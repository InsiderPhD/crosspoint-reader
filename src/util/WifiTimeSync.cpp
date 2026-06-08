#include "WifiTimeSync.h"

#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "CrossPointState.h"
#include "TimeUtils.h"
#include "WifiCredentialStore.h"

namespace {
// Silent boot worker. Connects to the last-known SSID, runs up to 3 NTP
// attempts, tears WiFi down, then deletes itself. Intentionally short timeouts
// so a missing/dead access point at boot doesn't keep the radio on forever.
//
// Conflict safety with user-initiated WiFi flows: if the user opens a
// WiFi-using activity (BookFusion sync, settings, etc.) before this task
// finishes, that activity's WifiSelectionActivity will tear down our
// connection on entry. We notice and bail out — its own onComplete hook then
// triggers attemptIfStale, so the user's path still gets a fresh clock.
void silentBootTask(void* /*arg*/) {
  // Let the UI finish its first paint before we start chewing CPU on WiFi.
  // setup() schedules us right before returning, so without this delay we'd
  // race the home-screen render for SD/SPI bandwidth on a cold boot.
  vTaskDelay(1500 / portTICK_PERIOD_MS);

  if (TimeUtils::wasTimeSyncedThisBoot()) {
    vTaskDelete(nullptr);
    return;
  }

  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("WTS", "Silent boot NTP: no last-connected SSID, skipping");
    vTaskDelete(nullptr);
    return;
  }

  const WifiCredential* cred = WIFI_STORE.findCredential(lastSsid);
  if (cred == nullptr) {
    LOG_DBG("WTS", "Silent boot NTP: no saved credentials for '%s'", lastSsid.c_str());
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
  for (int i = 0; i < kConnectMaxIters && WiFi.status() != WL_CONNECTED; ++i) {
    vTaskDelay(kConnectPollMs / portTICK_PERIOD_MS);
  }

  bool synced = false;
  if (WiFi.status() == WL_CONNECTED) {
    LOG_INF("WTS", "Silent boot WiFi connected, starting SNTP");
    // Up to 2 SNTP attempts, each with a 5s timeout (10s total). The previous
    // 2s × 3 = 6s wasn't enough on slow networks where DNS+NTP can exceed 5s.
    for (int attempt = 0; attempt < 2 && !synced; ++attempt) {
      synced = WifiTimeSync::attemptIfStale(5000);
      if (!synced) {
        LOG_INF("WTS", "Silent NTP attempt %d/2 failed", attempt + 1);
      }
    }
  } else {
    LOG_INF("WTS", "Silent boot WiFi connect failed (status=%d)", WiFi.status());
  }

  // Always tear WiFi back down — this is a silent boot helper, the radio
  // shouldn't stay on without the user knowing.
  WiFi.disconnect(false);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_OFF);

  if (synced) {
    LOG_INF("WTS", "Silent boot NTP succeeded");
  } else {
    LOG_INF("WTS", "Silent boot NTP gave up, continuing without fresh clock");
  }
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
  // 4 KB stack is enough for the WiFi/SNTP path. Priority 1 (background).
  xTaskCreate(&silentBootTask, "WTSBoot", 4096, nullptr, 1, nullptr);
}
