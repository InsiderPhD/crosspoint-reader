#include "WifiTimeSync.h"

#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointState.h"
#include "TimeUtils.h"
#include "WifiCredentialStore.h"

namespace {
// Preemption handshake with preempt(). Each flag has a single writer, so a plain
// volatile bool is sufficient (no ISR involvement, so no critical section needed).
//   sBootActive     — written by the task (true for its lifetime), read by preempt()
//   sStopRequested  — written by preempt(), read by the task at its yield points
volatile bool sBootActive = false;
volatile bool sStopRequested = false;

// Silent boot worker. Connects to the last-known SSID, runs up to 3 NTP
// attempts, tears WiFi down, then deletes itself. Intentionally short timeouts
// so a missing/dead access point at boot doesn't keep the radio on forever.
//
// Conflict safety with user-initiated WiFi flows: if the user opens a
// WiFi-using activity (BookFusion sync, settings, etc.) before this task
// finishes, that activity's WifiSelectionActivity will tear down our
// connection on entry. We notice and bail out — its own onComplete hook then
// triggers attemptIfStale, so the user's path still gets a fresh clock.
//
// Preemption: a heavy foreground job (library recache) can call preempt() to
// force an early exit so the two don't contend for heap/SD. We check
// sStopRequested at every yield point and tear WiFi down cleanly when set.
void silentBootTask(void* /*arg*/) {
  // sBootActive is set true by startSilentBootAttempt() before the task is
  // created, so a browse that preempts in the gap before this task first runs
  // still waits correctly rather than racing WiFi bring-up.

  // Let the UI finish its first paint before we start chewing CPU on WiFi.
  // setup() schedules us right before returning, so without this delay we'd
  // race the home-screen render for SD/SPI bandwidth on a cold boot.
  vTaskDelay(1500 / portTICK_PERIOD_MS);

  // If a browse preempted us during that delay, bail before touching the radio
  // (zero heap cost — WiFi was never brought up).
  if (sStopRequested || TimeUtils::wasTimeSyncedThisBoot()) {
    sBootActive = false;
    vTaskDelete(nullptr);
    return;
  }

  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("WTS", "Silent boot NTP: no last-connected SSID, skipping");
    sBootActive = false;
    vTaskDelete(nullptr);
    return;
  }

  const WifiCredential* cred = WIFI_STORE.findCredential(lastSsid);
  if (cred == nullptr) {
    LOG_DBG("WTS", "Silent boot NTP: no saved credentials for '%s'", lastSsid.c_str());
    sBootActive = false;
    vTaskDelete(nullptr);
    return;
  }

  LOG_INF("WTS", "Silent boot NTP attempt via '%s'", lastSsid.c_str());
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }

  // Wait up to ~8s for the association (corporate WiFi often takes >5s).
  constexpr int kConnectPollMs = 100;
  constexpr int kConnectMaxIters = 80;
  for (int i = 0; i < kConnectMaxIters && WiFi.status() != WL_CONNECTED && !sStopRequested; ++i) {
    vTaskDelay(kConnectPollMs / portTICK_PERIOD_MS);
  }

  bool synced = false;
  if (!sStopRequested && WiFi.status() == WL_CONNECTED) {
    LOG_INF("WTS", "Silent boot WiFi connected, starting SNTP");
    // Up to 2 SNTP attempts, each with a 5s timeout (10s total). The previous
    // 2s × 3 = 6s wasn't enough on slow networks where DNS+NTP can exceed 5s.
    // Re-check the stop flag before each attempt so preempt() only ever waits on
    // one in-flight SNTP call, not both.
    for (int attempt = 0; attempt < 2 && !synced && !sStopRequested; ++attempt) {
      synced = WifiTimeSync::attemptIfStale(5000);
      if (!synced) {
        LOG_INF("WTS", "Silent NTP attempt %d/2 failed", attempt + 1);
      }
    }
  } else if (!sStopRequested) {
    LOG_INF("WTS", "Silent boot WiFi connect failed (status=%d)", WiFi.status());
  }

  // Always tear WiFi back down — this is a silent boot helper, the radio
  // shouldn't stay on without the user knowing.
  WiFi.disconnect(false);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_OFF);

  if (sStopRequested) {
    LOG_INF("WTS", "Silent boot NTP preempted, WiFi torn down");
  } else if (synced) {
    LOG_INF("WTS", "Silent boot NTP succeeded");
  } else {
    LOG_INF("WTS", "Silent boot NTP gave up, continuing without fresh clock");
  }
  sBootActive = false;  // Last write: preempt() waits on this to know WiFi is down.
  vTaskDelete(nullptr);
}
}  // namespace

bool WifiTimeSync::attemptIfStale(const uint32_t timeoutMs) {
  // Already synced this boot — nothing to do, and re-running SNTP would
  // just stop/restart the daemon and waste cycles.
  if (TimeUtils::wasTimeSyncedThisBoot() && TimeUtils::isClockValid()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return TimeUtils::isClockValid();
  }

  if (!TimeUtils::syncTimeWithNtp(timeoutMs)) {
    LOG_DBG("WTS", "NTP sync attempt failed (timeout %ums)", timeoutMs);
    return TimeUtils::isClockValid();
  }

  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(now)) {
    // syncTimeWithNtp returned true but the clock still isn't past the
    // 2024-01-01 threshold. Treat as failure — don't poison the persisted
    // lastKnownValidTimestamp with a bogus value.
    return false;
  }

  APP_STATE.registerValidTimeSync(now);
  APP_STATE.saveToFile();
  LOG_INF("WTS", "NTP synced, lastKnownValidTimestamp=%u", now);
  return true;
}

void WifiTimeSync::startSilentBootAttempt() {
  if (TimeUtils::wasTimeSyncedThisBoot()) {
    return;
  }
  // The store is normally loaded by WifiSelectionActivity::onEnter — load it
  // here too so we don't depend on a previous activity having run.
  WIFI_STORE.loadFromFile();
  if (WIFI_STORE.getLastConnectedSsid().empty()) {
    // No saved network — don't bother spawning a task just to no-op.
    return;
  }
  // Clear any stale preempt request, and mark active *before* the task exists so
  // an early preempt() can't slip through the create-to-first-run gap.
  sStopRequested = false;
  sBootActive = true;
  // 4 KB stack is enough for the WiFi/SNTP path. Priority 1 (background).
  if (xTaskCreate(&silentBootTask, "WTSBoot", 4096, nullptr, 1, nullptr) != pdPASS) {
    sBootActive = false;
    LOG_ERR("WTS", "Failed to create silent boot NTP task");
  }
}

void WifiTimeSync::preempt(const uint32_t maxWaitMs) {
  if (!sBootActive) return;  // Task already finished (or never started) — nothing to do.

  LOG_INF("WTS", "Preempting silent boot NTP for a foreground job");
  sStopRequested = true;

  // Wait until the task has actually released the radio (sBootActive == false),
  // so the caller's heavy allocations don't overlap the WiFi stack's ~40KB.
  constexpr uint32_t kPollMs = 20;
  uint32_t waited = 0;
  while (sBootActive && waited < maxWaitMs) {
    vTaskDelay(kPollMs / portTICK_PERIOD_MS);
    waited += kPollMs;
  }
  if (sBootActive) {
    LOG_ERR("WTS", "Preempt timed out after %ums; proceeding with WiFi still up", waited);
  } else {
    LOG_DBG("WTS", "Preempt complete in ~%ums", waited);
  }
}
