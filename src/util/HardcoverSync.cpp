#include "HardcoverSync.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HardcoverTokenStore.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "TimeUtils.h"

#if CROSSPOINT_HARDCOVER_AUTO_SYNC
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "WifiCredentialStore.h"
#endif

namespace {

// Everything a push reads, owned, so it can cross to the background task after
// the stats record it came from has changed or gone.
struct Job {
  std::string path;
  std::string isbnPath;
  std::string title;
  std::string author;
  float percent = 0.0f;
  bool finished = false;
  char today[11] = {};        // YYYY-MM-DD, empty when the clock isn't valid
  uint32_t todayOrdinal = 0;  // 0 when the clock isn't valid
  std::vector<HardcoverDayProgress> days;
};

bool formatDayOrdinal(const uint32_t dayOrdinal, char* out, const size_t outLen) {
  out[0] = '\0';
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (dayOrdinal == 0 || !TimeUtils::getDateFromDayOrdinal(dayOrdinal, year, month, day)) return false;
  snprintf(out, outLen, "%04d-%02u-%02u", year, month, day);
  return true;
}

// One entry per dated day this book was read on, carrying the end progress of
// that day's last logged session. The log is in append order, so a later
// session on the same day overwrites an earlier one. Sessions logged before
// progress was recorded, or never dated, are skipped.
void collectDays(const ReadingBookStats& book, std::vector<HardcoverDayProgress>& out) {
  if (book.bookId.empty()) return;
  const auto& log = READING_STATS.getSessionLog();
  size_t count = 0;
  for (const auto& session : log) {
    if (session.bookId == book.bookId) count++;
  }
  // Upper bound (one per session); the log is capped at 256 entries in total.
  out.reserve(count);
  for (const auto& session : log) {
    if (session.bookId != book.bookId || session.dayOrdinal == 0 ||
        session.endProgressPercent == ReadingSessionLogEntry::PROGRESS_UNKNOWN) {
      continue;
    }
    auto existing = std::find_if(out.begin(), out.end(), [&session](const HardcoverDayProgress& day) {
      return day.dayOrdinal == session.dayOrdinal;
    });
    if (existing != out.end()) {
      existing->percent = session.endProgressPercent;
      continue;
    }
    HardcoverDayProgress day;
    day.dayOrdinal = session.dayOrdinal;
    day.percent = session.endProgressPercent;
    if (formatDayOrdinal(day.dayOrdinal, day.date, sizeof(day.date))) {
      out.push_back(day);
    }
  }
  // Re-dated sessions (Sessions tab) can land out of order.
  std::sort(out.begin(), out.end(),
            [](const HardcoverDayProgress& a, const HardcoverDayProgress& b) { return a.dayOrdinal < b.dayOrdinal; });
}

Job makeJob(const ReadingBookStats& book) {
  Job job;
  job.path = book.path;
  // The ISBN sidecar lives in the EPUB's cache directory; constructing an Epub
  // only derives that path, it reads nothing.
  if (FsHelpers::hasEpubExtension(book.path)) {
    job.isbnPath = Epub(book.path, "/.crosspoint").getIsbnPath();
  }
  job.title = book.title;
  job.author = book.author;
  job.percent = static_cast<float>(book.lastProgressPercent);
  job.finished = book.completed;
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  if (TimeUtils::isClockValid(now)) {
    job.todayOrdinal = TimeUtils::getLocalDayOrdinal(now);
    if (!formatDayOrdinal(job.todayOrdinal, job.today, sizeof(job.today))) {
      job.todayOrdinal = 0;
    }
  }
  collectDays(book, job.days);
  return job;
}

HardcoverSyncClient::Error runJob(const Job& job, HardcoverPushResult* out) {
  HardcoverBookInfo info;
  info.epubPath = job.path.c_str();
  info.isbnPath = job.isbnPath.empty() ? nullptr : job.isbnPath.c_str();
  info.title = job.title.c_str();
  info.author = job.author.c_str();
  info.days = job.days.data();
  info.dayCount = job.days.size();
  info.todayOrdinal = job.todayOrdinal;
  return HardcoverSyncClient::pushProgress(info, job.percent, job.finished, job.today[0] ? job.today : nullptr, out);
}

}  // namespace

namespace HardcoverSync {

bool isEligible(const ReadingBookStats& book) {
  if (book.path.empty() || ReadingStatsStore::shouldIgnorePath(book.path)) return false;
  return book.lastProgressPercent > 0 || book.completed;
}

bool isUpToDate(const ReadingBookStats& book) {
  return HardcoverSyncClient::isUpToDate(book.path.c_str(), static_cast<float>(book.lastProgressPercent),
                                         book.completed);
}

HardcoverSyncClient::Error pushBook(const ReadingBookStats& book, HardcoverPushResult* out) {
  return runJob(makeJob(book), out);
}

void forgetSyncState(const ReadingBookStats& book) { HardcoverSyncClient::forgetSyncState(book.path.c_str()); }

}  // namespace HardcoverSync

#if CROSSPOINT_HARDCOVER_AUTO_SYNC

namespace {

// Background worker state. The worker exists only while there is work: it is
// created by onSessionWritten and deletes itself once the queue slot is empty.
// sMutex guards the slot and sActive; the two stop flags each have one writer.
SemaphoreHandle_t sMutex = nullptr;
Job sPending;
bool sHasPending = false;
volatile bool sActive = false;          // written under sMutex; preempt() polls it unlocked
volatile bool sStopRequested = false;   // preempt(): abandon the work
volatile bool sSleepRequested = false;  // finishBeforeSleep(): skip the settle delay

// Worker stack. The TLS transport is sized for the 8KB loop task (its receive
// buffer is 2KB on the stack); the headroom covers ArduinoJson and wolfSSL's
// handshake frames. Internal SRAM on the S3 is not the constraint the C3 has.
constexpr uint32_t kWorkerStackBytes = 12 * 1024;

// Let the activity that follows the reader (Home, or the sleep screen) paint
// before WiFi and SD work start competing with it.
constexpr uint32_t kSettleMs = 2000;
constexpr uint32_t kPollMs = 100;
constexpr int kConnectMaxIters = 80;  // ~8s, as WifiTimeSync (slow APs take >5s)

bool takeJob(Job& out) {
  xSemaphoreTake(sMutex, portMAX_DELAY);
  const bool have = sHasPending && !sStopRequested;
  if (have) {
    out = std::move(sPending);
    sHasPending = false;
  } else {
    sHasPending = false;
    sActive = false;  // last write under the lock: a new request spawns a new task
  }
  xSemaphoreGive(sMutex);
  return have;
}

// Bring WiFi up on the last-known network, as WifiTimeSync's boot task does.
bool connectSilently() {
  WIFI_STORE.loadFromFile();
  const std::string& ssid = WIFI_STORE.getLastConnectedSsid();
  const WifiCredential* cred = ssid.empty() ? nullptr : WIFI_STORE.findCredential(ssid);
  if (cred == nullptr) {
    LOG_DBG("HCS", "Background push: no saved network");
    return false;
  }
  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  for (int i = 0; i < kConnectMaxIters && WiFi.status() != WL_CONNECTED && !sStopRequested; ++i) {
    vTaskDelay(kPollMs / portTICK_PERIOD_MS);
  }
  return !sStopRequested && WiFi.status() == WL_CONNECTED;
}

void teardownWifi() {
  WiFi.disconnect(false);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_OFF);
}

void runBackgroundJob(const Job& job) {
  // Someone else's live connection is used as-is and left up. A radio that is
  // on but not connected belongs to a foreground flow mid-connect: leave it
  // alone. The book stays unsynced, so the next session end (or the Stats
  // screen) picks it up.
  bool ownsWifi = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (WiFi.getMode() != WIFI_OFF) {
      LOG_INF("HCS", "Background push skipped: WiFi in use");
      return;
    }
    ownsWifi = true;
    if (!connectSilently()) {
      LOG_INF("HCS", "Background push skipped: WiFi connect failed");
      teardownWifi();
      return;
    }
  }

  const auto err = runJob(job, nullptr);
  if (err == HardcoverSyncClient::OK) {
    LOG_INF("HCS", "Background push done: %s", job.title.c_str());
  } else {
    LOG_INF("HCS", "Background push failed: %s", HardcoverSyncClient::errorString(err));
  }

  if (ownsWifi) {
    teardownWifi();
  }
}

void workerTask(void* /*arg*/) {
  for (uint32_t waited = 0; waited < kSettleMs && !sStopRequested && !sSleepRequested; waited += kPollMs) {
    vTaskDelay(kPollMs / portTICK_PERIOD_MS);
  }

  Job job;
  while (takeJob(job)) {
    runBackgroundJob(job);
  }
  vTaskDelete(nullptr);
}

void waitForWorker(const uint32_t maxWaitMs, const char* why) {
  uint32_t waited = 0;
  while (sActive && waited < maxWaitMs) {
    vTaskDelay(kPollMs / portTICK_PERIOD_MS);
    waited += kPollMs;
  }
  if (sActive) {
    LOG_ERR("HCS", "Background push still running after %ums (%s)", waited, why);
  } else if (waited > 0) {
    LOG_DBG("HCS", "Background push released after ~%ums (%s)", waited, why);
  }
}

}  // namespace

namespace HardcoverSync {

void onSessionWritten(const ReadingBookStats& book) {
  if (!HC_TOKEN_STORE.hasToken() || !isEligible(book) || isUpToDate(book)) {
    return;
  }
  if (sMutex == nullptr) {
    sMutex = xSemaphoreCreateMutex();
    if (sMutex == nullptr) {
      LOG_ERR("HCS", "Background push mutex allocation failed");
      return;
    }
  }

  Job job = makeJob(book);
  xSemaphoreTake(sMutex, portMAX_DELAY);
  sPending = std::move(job);
  sHasPending = true;
  const bool spawn = !sActive;
  if (spawn) {
    sActive = true;
    sStopRequested = false;
    sSleepRequested = false;
  }
  xSemaphoreGive(sMutex);

  if (spawn && xTaskCreate(&workerTask, "HardcoverSync", kWorkerStackBytes, nullptr, 1, nullptr) != pdPASS) {
    LOG_ERR("HCS", "Failed to create background push task");
    xSemaphoreTake(sMutex, portMAX_DELAY);
    sActive = false;
    sHasPending = false;
    xSemaphoreGive(sMutex);
  }
}

void preempt(const uint32_t maxWaitMs) {
  if (!sActive) return;
  LOG_INF("HCS", "Preempting background push for a foreground WiFi user");
  sStopRequested = true;
  waitForWorker(maxWaitMs, "preempt");
}

void finishBeforeSleep(const uint32_t maxWaitMs) {
  if (!sActive) return;
  LOG_INF("HCS", "Waiting for background push before sleep");
  sSleepRequested = true;
  waitForWorker(maxWaitMs, "sleep");
}

}  // namespace HardcoverSync

#endif  // CROSSPOINT_HARDCOVER_AUTO_SYNC
