#include "ProgressAutoSync.h"

#include <BookFusionBookIdStore.h>
#include <BookFusionTokenStore.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <KOReaderSyncStateStore.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "TimeUtils.h"
#include "WifiCredentialStore.h"

// Implemented in EpubReaderActivity.cpp — shared so the timestamp format stays
// byte-identical to the manual sync paths (BookFusion sorts updated_at as a
// string, so a divergent format would break conflict resolution).
extern bool formatLocalSyncTimestamp(char* out, size_t outLen);
extern bool localEpochSeconds(int64_t& outEpoch);
extern BookFusionStoredPosition storedPositionFromBookFusion(const BookFusionPosition& pos);

namespace {

// Sanity floor checked AFTER the caller has released the section and Epub. The
// manual sync reaches its handshake with ~61KB free and bottoms out around
// 11KB, so anything below this means the release didn't recover what it should
// and pushing would risk an OOM instead of a clean skip.
constexpr size_t kMinFreeHeapAfterRelease = 45 * 1024;

// Minimum gap between attempts. Bounds retry-on-failure and stops a book of
// many tiny spine items from re-syncing every chapter.
constexpr uint32_t kCooldownMs = 60000;

// Association wait, matching WifiTimeSync's silent boot task (corporate APs
// routinely take >5s).
constexpr int kConnectPollMs = 100;
constexpr int kConnectMaxIters = 80;

bool sArmed = false;
uint32_t sLastAttemptMs = 0;

// Per-book provider cache, so a page turn costs no SD I/O after the first call.
std::string sProviderPath;
ProgressAutoSync::Provider sProvider = ProgressAutoSync::Provider::None;
uint32_t sProviderBookId = 0;

// Per-book progress baseline. Seeded from the last-synced sidecar when there is
// one, otherwise from the position at first check — so opening a book never
// arms a sync immediately.
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
  // it just means the delta rolls into the next sync. The connection is already
  // up, so this second request has no handshake spike.
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

// Bring WiFi up on the last-known network. Deliberately short timeouts so a
// dead AP doesn't hold the reader hostage.
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
  for (int i = 0; i < kConnectMaxIters && WiFi.status() != WL_CONNECTED; ++i) {
    vTaskDelay(kConnectPollMs / portTICK_PERIOD_MS);
  }
  return WiFi.status() == WL_CONNECTED;
}

void teardownWifi() {
  WiFi.disconnect(false);
  vTaskDelay(50 / portTICK_PERIOD_MS);
  WiFi.mode(WIFI_OFF);
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

bool armIfThresholdCrossed(const std::string& epubPath, const int spineIndex, const float bookPercent) {
  const uint8_t mode = SETTINGS.autosyncMode;
  if (mode == CrossPointSettings::AUTOSYNC_OFF || mode == CrossPointSettings::AUTOSYNC_ON_EXIT) {
    return false;
  }
  if (sArmed) {
    return true;  // already waiting for page 2
  }
  // The boot NTP attempt is our proof that WiFi works this session; without a
  // valid clock the sync timestamps would be meaningless anyway.
  if (!TimeUtils::wasTimeSyncedThisBoot()) {
    return false;
  }
  const uint32_t now = millis();
  if (sLastAttemptMs != 0 && (now - sLastAttemptMs) < kCooldownMs) {
    return false;
  }

  resolveProvider(epubPath);
  if (sProvider == Provider::None) {
    return false;
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
    return false;
  }

  LOG_DBG("PAS", "Autosync armed at %.1f%% (baseline %.1f%%)", bookPercent, sBaselinePercent);
  sArmed = true;
  return true;
}

bool isArmed() { return sArmed; }

void disarm() { sArmed = false; }

bool heapAllowsPush() {
  const size_t freeHeap = esp_get_free_heap_size();
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (freeHeap < kMinFreeHeapAfterRelease) {
    LOG_DBG("PAS", "Skipping push: only %u free after release, %u largest (need %u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largest), static_cast<unsigned>(kMinFreeHeapAfterRelease));
    return false;
  }
  LOG_DBG("PAS", "Push heap after release: %u free, %u largest", static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(largest));
  return true;
}

bool pushBlocking(const Payload& payload) {
  if (payload.provider == Provider::None) {
    return false;
  }
  sArmed = false;
  sLastAttemptMs = millis();

  if (!connectSilently()) {
    teardownWifi();
    return false;
  }

  bool pushed = false;
  switch (payload.provider) {
    case Provider::BookFusion:
      pushed = pushBookFusion(payload);
      break;
    case Provider::KOReader:
      pushed = pushKOReader(payload);
      break;
    default:
      break;
  }

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
  sArmed = false;
  sBaselineValid = false;
  sBaselinePercent = 0.0f;
  sBaselineSpine = 0;
  sProviderPath.clear();
  sProvider = Provider::None;
  sProviderBookId = 0;
}

}  // namespace ProgressAutoSync
