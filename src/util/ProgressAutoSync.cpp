#include "ProgressAutoSync.h"

#include <BookFusionBookIdStore.h>
#include <BookFusionTokenStore.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <KOReaderSyncStateStore.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "TimeUtils.h"
#include "WifiCredentialStore.h"
#include "WifiTimeSync.h"

// Implemented in EpubReaderActivity.cpp — shared so the timestamp format stays
// byte-identical to the manual sync paths (BookFusion sorts updated_at as a
// string, so a divergent format would break conflict resolution).
extern bool formatLocalSyncTimestamp(char* out, size_t outLen);
extern bool localEpochSeconds(int64_t& outEpoch);
extern BookFusionStoredPosition storedPositionFromBookFusion(const BookFusionPosition& pos);

namespace {

// Heap required before we're willing to bring WiFi up underneath an active
// reader. WiFi + lwIP is ~40-45KB and a TLS handshake peaks ~35KB on top of
// that; the remaining ~30KB is the envelope the reader is known to render in
// (KOReaderSyncClient documents "~46KB free after WiFi" as the working point).
// The contiguous check is the one that actually matters in practice: a
// fragmented heap fails the 16KB TLS record buffer even when total free looks
// healthy. TUNABLE — validate against the MEMDIAG-style logs below on device.
constexpr size_t kMinFreeHeap = 110 * 1024;
constexpr size_t kMinLargestBlock = 32 * 1024;

// Minimum gap between attempts. Bounds retry-on-failure and stops a book of
// many tiny spine items from hammering the radio in Every Chapter mode.
constexpr uint32_t kCooldownMs = 60000;

// Association wait, matching WifiTimeSync's silent boot task (corporate APs
// routinely take >5s).
constexpr int kConnectPollMs = 100;
constexpr int kConnectMaxIters = 80;

// Preemption handshake, same single-writer discipline as WifiTimeSync:
//   sTaskActive    — written by the task (and by start() before creation),
//                    read by preempt()/isBusy()
//   sStopRequested — written by preempt(), read by the task at its yield points
//   sCompleted     — written by the task, consumed by the reader's loop()
volatile bool sTaskActive = false;
volatile bool sStopRequested = false;
volatile bool sCompleted = false;

uint32_t sLastAttemptMs = 0;

// Per-book provider cache, so a page turn costs no SD I/O after the first call.
std::string sProviderPath;
ProgressAutoSync::Provider sProvider = ProgressAutoSync::Provider::None;
uint32_t sProviderBookId = 0;

// Per-book progress baseline. Seeded from the last-synced sidecar when there is
// one, otherwise from the position at first check — so opening a book never
// fires an immediate sync.
bool sBaselineValid = false;
float sBaselinePercent = 0.0f;
int sBaselineSpine = 0;

// KOReader binary document hash cache (the partial MD5 reads the EPUB off SD).
std::string sHashPath;
std::string sHash;

void resolveProvider(const std::string& epubPath) {
  if (sProviderPath == epubPath && !epubPath.empty()) {
    return;
  }
  sProviderPath = epubPath;
  sProvider = ProgressAutoSync::Provider::None;
  sProviderBookId = 0;
  if (epubPath.empty()) {
    return;
  }

  // Same precedence as the manual sync paths: a BookFusion-linked book with a
  // token syncs to BookFusion, everything else falls to KOReader.
  const uint32_t bookId = BookFusionBookIdStore::loadBookId(epubPath.c_str());
  if (bookId != 0 && BF_TOKEN_STORE.hasToken()) {
    sProvider = ProgressAutoSync::Provider::BookFusion;
    sProviderBookId = bookId;
    return;
  }
  if (KOREADER_STORE.hasCredentials()) {
    sProvider = ProgressAutoSync::Provider::KOReader;
  }
}

// Seed the RAM baseline from whatever the last successful sync persisted.
void seedBaseline(const std::string& epubPath, const int spineIndex, const float bookPercent) {
  if (sBaselineValid) {
    return;
  }
  sBaselinePercent = bookPercent;
  sBaselineSpine = spineIndex;

  if (sProvider == ProgressAutoSync::Provider::BookFusion) {
    BookFusionStoredPosition stored;
    if (BookFusionBookIdStore::loadLastSyncedPosition(epubPath.c_str(), stored)) {
      sBaselinePercent = stored.percentage;
      sBaselineSpine = stored.chapterIndex;
    }
  } else if (sProvider == ProgressAutoSync::Provider::KOReader) {
    KOReaderStoredPosition stored;
    if (KOReaderSyncStateStore::loadLastSyncedPosition(epubPath.c_str(), stored)) {
      sBaselinePercent = stored.percentage * 100.0f;  // sidecar stores 0.0-1.0
      sBaselineSpine = stored.spineIndex;
    }
  }
  sBaselineValid = true;
}

bool heapAllowsSync() {
  const size_t freeHeap = esp_get_free_heap_size();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < kMinFreeHeap || largest < kMinLargestBlock) {
    LOG_DBG("PAS", "Skipping autosync: heap %u free, %u largest (need %u/%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largest), static_cast<unsigned>(kMinFreeHeap),
            static_cast<unsigned>(kMinLargestBlock));
    return false;
  }
  LOG_DBG("PAS", "Autosync gate passed: heap %u free, %u largest", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(largest));
  return true;
}

// Resolve the KOReader document hash, caching the expensive binary variant.
std::string koDocumentHash(const std::string& epubPath) {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(epubPath);
  }
  if (sHashPath == epubPath && !sHash.empty()) {
    return sHash;
  }
  std::string hash = KOReaderDocumentId::calculate(epubPath);
  if (!hash.empty()) {
    sHashPath = epubPath;
    sHash = hash;
  }
  return hash;
}

bool pushBookFusion(const ProgressAutoSync::Payload& payload) {
  BookFusionPosition uploaded = payload.bfPos;
  const auto result = BookFusionSyncClient::setProgress(payload.bookId, payload.bfPos, &uploaded);
  if (result != BookFusionSyncClient::OK) {
    LOG_DBG("PAS", "BookFusion push failed: %s", BookFusionSyncClient::errorString(result));
    return false;
  }

  const char* path = payload.epubPath.c_str();
  if (uploaded.updatedAt[0] != '\0') {
    BookFusionBookIdStore::saveLastSyncAt(path, uploaded.updatedAt);
  } else {
    char localTs[40];
    if (formatLocalSyncTimestamp(localTs, sizeof(localTs))) {
      BookFusionBookIdStore::saveLastSyncAt(path, localTs);
    }
  }
  BookFusionStoredPosition stored = storedPositionFromBookFusion(uploaded);
  stored.pageNumber = payload.pageNumber;
  stored.totalPages = payload.totalPages;
  BookFusionBookIdStore::saveLastSyncedPosition(path, stored);

  // Reading-time delta is best-effort: a failure here doesn't fail the push,
  // it just means the delta rolls into the next sync.
  if (payload.totalReadingMs > 0) {
    const uint64_t lastSyncedMs = BookFusionBookIdStore::loadLastSyncedReadingMs(path);
    if (payload.totalReadingMs > lastSyncedMs) {
      const uint32_t durationSeconds = static_cast<uint32_t>((payload.totalReadingMs - lastSyncedMs) / 1000u);
      if (durationSeconds >= 5) {
        char loggedAt[40] = {};
        if (formatLocalSyncTimestamp(loggedAt, sizeof(loggedAt))) {
          const auto trackResult = BookFusionSyncClient::trackReadingTime(payload.bookId, durationSeconds, loggedAt);
          if (trackResult == BookFusionSyncClient::OK) {
            BookFusionBookIdStore::saveLastSyncedReadingMs(path, payload.totalReadingMs);
          } else {
            LOG_DBG("PAS", "Reading time tracking failed (non-fatal): %s",
                    BookFusionSyncClient::errorString(trackResult));
          }
        }
      }
    }
  }

  LOG_INF("PAS", "BookFusion autosync pushed %.1f%%", payload.bfPos.percentage);
  return true;
}

bool pushKOReader(const ProgressAutoSync::Payload& payload) {
  std::string hash = payload.koDocumentHash;
  if (hash.empty()) {
    hash = koDocumentHash(payload.epubPath);
  }
  if (hash.empty()) {
    LOG_DBG("PAS", "KOReader autosync: could not compute document hash");
    return false;
  }

  KOReaderProgress progress;
  progress.document = hash;
  progress.progress = payload.koXpath;
  progress.percentage = payload.koPercentage;
  const auto result = KOReaderSyncClient::updateProgress(progress);
  if (result != KOReaderSyncClient::OK) {
    LOG_DBG("PAS", "KOReader push failed: %s", KOReaderSyncClient::errorString(result));
    return false;
  }

  const char* path = payload.epubPath.c_str();
  int64_t localTs = 0;
  if (localEpochSeconds(localTs)) {
    KOReaderSyncStateStore::saveLastSyncTimestamp(path, localTs);
  }
  KOReaderStoredPosition stored;
  stored.percentage = payload.koPercentage;
  stored.spineIndex = payload.spineIndex;
  stored.pageNumber = payload.pageNumber;
  stored.totalPages = payload.totalPages;
  KOReaderSyncStateStore::saveLastSyncedPosition(path, stored);

  LOG_INF("PAS", "KOReader autosync pushed %.1f%%", payload.koPercentage * 100.0f);
  return true;
}

// Bring WiFi up on the last-known network. Mirrors WifiTimeSync::silentBootTask
// — deliberately short timeouts so a dead AP doesn't hold the radio on.
bool connectSilently() {
  WIFI_STORE.loadFromFile();
  const std::string& lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    LOG_DBG("PAS", "No last-connected SSID, skipping autosync");
    return false;
  }
  const WifiCredential* cred = WIFI_STORE.findCredential(lastSsid);
  if (cred == nullptr) {
    LOG_DBG("PAS", "No saved credentials for '%s'", lastSsid.c_str());
    return false;
  }

  WiFi.mode(WIFI_STA);
  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }
  for (int i = 0; i < kConnectMaxIters && WiFi.status() != WL_CONNECTED && !sStopRequested; ++i) {
    vTaskDelay(kConnectPollMs / portTICK_PERIOD_MS);
  }
  return !sStopRequested && WiFi.status() == WL_CONNECTED;
}

void teardownWifi() {
  WiFi.disconnect(false);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_OFF);
}

bool performPush(const ProgressAutoSync::Payload& payload) {
  switch (payload.provider) {
    case ProgressAutoSync::Provider::BookFusion:
      return pushBookFusion(payload);
    case ProgressAutoSync::Provider::KOReader:
      return pushKOReader(payload);
    default:
      return false;
  }
}

void autosyncTask(void* arg) {
  // Take ownership immediately so every exit path below frees the payload.
  std::unique_ptr<ProgressAutoSync::Payload> payload(static_cast<ProgressAutoSync::Payload*>(arg));

  // The boot NTP task flips wasTimeSyncedThisBoot() before it finishes tearing
  // WiFi down, so we can arrive here while it still owns the radio.
  WifiTimeSync::preempt(2000);

  bool pushed = false;
  if (!sStopRequested && payload && connectSilently()) {
    if (!sStopRequested) {
      pushed = performPush(*payload);
      if (pushed) {
        sBaselinePercent = payload->bookPercent;
        sBaselineSpine = payload->spineIndex;
      }
    }
    teardownWifi();
  } else if (!sStopRequested) {
    // connectSilently() may have left the radio half-up on a failed association.
    teardownWifi();
  } else {
    teardownWifi();
    LOG_DBG("PAS", "Autosync preempted");
  }

  sCompleted = true;
  sTaskActive = false;  // Last write: preempt() waits on this to know WiFi is down.
  vTaskDelete(nullptr);
}

}  // namespace

namespace ProgressAutoSync {

Trigger providerFor(const std::string& epubPath) {
  Trigger trigger;
  resolveProvider(epubPath);
  trigger.provider = sProvider;
  trigger.bookId = sProviderBookId;
  return trigger;
}

Trigger shouldSync(const std::string& epubPath, const int spineIndex, const float bookPercent) {
  Trigger trigger;

  const uint8_t mode = SETTINGS.autosyncMode;
  if (mode == CrossPointSettings::AUTOSYNC_OFF || mode == CrossPointSettings::AUTOSYNC_ON_EXIT) {
    return trigger;
  }
  // The boot NTP attempt is our proof that WiFi works this session; without a
  // valid clock the sync timestamps would be meaningless anyway.
  if (!TimeUtils::wasTimeSyncedThisBoot()) {
    return trigger;
  }
  if (sTaskActive) {
    return trigger;
  }
  const uint32_t now = millis();
  if (sLastAttemptMs != 0 && (now - sLastAttemptMs) < kCooldownMs) {
    return trigger;
  }

  resolveProvider(epubPath);
  if (sProvider == Provider::None) {
    return trigger;
  }

  seedBaseline(epubPath, spineIndex, bookPercent);

  // Forward-only. Autosync is push-only, so firing on a backward move would
  // clobber the server position and defeat the manual sync's re-read detection.
  bool crossed = false;
  if (mode == CrossPointSettings::AUTOSYNC_EVERY_CHAPTER) {
    crossed = spineIndex > sBaselineSpine;
  } else {
    const uint8_t step = SETTINGS.getAutosyncPercentStep();
    crossed = step > 0 && bookPercent >= sBaselinePercent + static_cast<float>(step);
  }
  if (!crossed) {
    return trigger;
  }

  if (!heapAllowsSync()) {
    return trigger;
  }

  trigger.provider = sProvider;
  trigger.bookId = sProviderBookId;
  return trigger;
}

bool start(std::unique_ptr<Payload> payload) {
  if (!payload || payload->provider == Provider::None) {
    return false;
  }
  if (sTaskActive) {
    return false;
  }

  sStopRequested = false;
  sCompleted = false;
  sLastAttemptMs = millis();
  // Mark active before the task exists so an early preempt() can't slip through
  // the create-to-first-run gap.
  sTaskActive = true;

  // 8KB: the mbedTLS handshake and JSON parsing run on this stack. WifiTimeSync
  // gets away with 4KB because SNTP does neither.
  if (xTaskCreate(&autosyncTask, "PASync", 8192, payload.get(), 1, nullptr) != pdPASS) {
    sTaskActive = false;
    LOG_ERR("PAS", "Failed to create autosync task");
    return false;
  }
  payload.release();  // the task owns it now
  return true;
}

bool isBusy() { return sTaskActive; }

bool consumeCompletion() {
  if (!sCompleted) {
    return false;
  }
  sCompleted = false;
  return true;
}

void preempt(const uint32_t maxWaitMs) {
  if (!sTaskActive) return;

  LOG_INF("PAS", "Preempting autosync");
  sStopRequested = true;

  constexpr uint32_t kPollMs = 20;
  uint32_t waited = 0;
  while (sTaskActive && waited < maxWaitMs) {
    vTaskDelay(kPollMs / portTICK_PERIOD_MS);
    waited += kPollMs;
  }
  if (sTaskActive) {
    LOG_ERR("PAS", "Autosync preempt timed out after %ums", static_cast<unsigned>(waited));
  }
}

bool pushBlocking(const Payload& payload) {
  if (payload.provider == Provider::None) {
    return false;
  }
  sLastAttemptMs = millis();
  if (!connectSilently()) {
    teardownWifi();
    return false;
  }
  const bool pushed = performPush(payload);
  teardownWifi();
  if (pushed) {
    sBaselinePercent = payload.bookPercent;
    sBaselineSpine = payload.spineIndex;
  }
  return pushed;
}

bool hasUnsyncedProgress(const Payload& payload) {
  const char* path = payload.epubPath.c_str();

  if (payload.provider == Provider::BookFusion) {
    BookFusionStoredPosition stored;
    if (!BookFusionBookIdStore::loadLastSyncedPosition(path, stored)) {
      return true;  // never synced — anything is progress
    }
    if (payload.spineIndex != stored.chapterIndex) {
      return payload.spineIndex > stored.chapterIndex;
    }
    if (stored.pageNumber >= 0 && stored.totalPages == payload.totalPages && payload.totalPages > 0) {
      return payload.pageNumber > stored.pageNumber;
    }
    static constexpr float kPercentEpsilon = 0.05f;
    return payload.bfPos.percentage > stored.percentage + kPercentEpsilon;
  }

  if (payload.provider == Provider::KOReader) {
    KOReaderStoredPosition stored;
    if (!KOReaderSyncStateStore::loadLastSyncedPosition(path, stored)) {
      return true;
    }
    if (payload.spineIndex != stored.spineIndex) {
      return payload.spineIndex > stored.spineIndex;
    }
    if (stored.pageNumber >= 0 && stored.totalPages == payload.totalPages && payload.totalPages > 0) {
      return payload.pageNumber > stored.pageNumber;
    }
    static constexpr float kKoPercentEpsilon = 0.0005f;
    return payload.koPercentage > stored.percentage + kKoPercentEpsilon;
  }

  return false;
}

void resetSessionBaseline() {
  sBaselineValid = false;
  sBaselinePercent = 0.0f;
  sBaselineSpine = 0;
  sProviderPath.clear();
  sProvider = Provider::None;
  sProviderBookId = 0;
}

}  // namespace ProgressAutoSync
