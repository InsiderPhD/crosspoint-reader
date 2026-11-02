#include "StatsSync.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "ReadingStatsStore.h"
#include "StatsPayload.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "util/TimeUtils.h"

namespace StatsSync {
namespace {
// One buffer for every request of the step. The overall snapshot is ~1.1KB
// (124-char history + 20 daily entries); a book item with 7 daily entries is
// ~550 bytes, so a request carries about 7 books.
constexpr size_t PAYLOAD_CAPACITY = 4096;
// The answers are tiny ({"until":N,...}); anything longer is not ours.
constexpr size_t REPLY_CAPACITY = 256;
// Daily entries per book: the newest week. The overall snapshot sends 20.
constexpr size_t BOOK_DAILY = 7;
constexpr char DEVICE_NAME[] = "KatiePoint";

std::string documentIdFor(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

uint32_t msToSeconds(const uint64_t ms) { return static_cast<uint32_t>(std::min<uint64_t>(ms / 1000, UINT32_MAX)); }

Result fromError(const KOReaderSyncClient::Error err) {
  switch (err) {
    case KOReaderSyncClient::OK:
      return Result::Ok;
    case KOReaderSyncClient::EXTENSION_UNSUPPORTED:
      return Result::Unsupported;
    case KOReaderSyncClient::LOW_MEMORY:
      return Result::LowMemory;
    default:
      return Result::Failed;
  }
}

// PUT inside a framebuffer borrow; OK only for a 2xx carrying "until" (a
// catch-all 200 page from a proxy is not a stats server).
Result put(GfxRenderer& renderer, const char* endpoint, const char* payload) {
  char reply[REPLY_CAPACITY];
  KOReaderSyncClient::Error err;
  {
    TlsFramebufferBorrow borrow(renderer);
    err = KOReaderSyncClient::extensionRequest(endpoint, payload, reply, sizeof(reply));
  }
  if (err != KOReaderSyncClient::OK) {
    LOG_ERR("STATSYNC", "%s failed: %s", endpoint, KOReaderSyncClient::errorString(err));
    return fromError(err);
  }
  JsonDocument doc;
  if (deserializeJson(doc, reply) != DeserializationError::Ok || !doc["until"].is<uint64_t>()) {
    LOG_ERR("STATSYNC", "%s: reply is not the stats API", endpoint);
    return Result::Unsupported;
  }
  return Result::Ok;
}

void fillDow(const std::vector<ReadingDayStats>& days, uint32_t (&dow)[7]) {
  for (const auto& day : days) {
    if (day.dayOrdinal == 0) continue;
    dow[StatsPayload::mondayIndex(day.dayOrdinal)] += msToSeconds(day.readingMs);
  }
}

// The newest `max` dated days of `days` (ascending), into `out`.
size_t newestDays(const std::vector<ReadingDayStats>& days, StatsPayload::DayValue* out, const size_t max) {
  size_t n = 0;
  for (auto it = days.rbegin(); it != days.rend() && n < max; ++it) {
    if (it->dayOrdinal == 0 || it->readingMs == 0) continue;
    out[n++] = {it->dayOrdinal, msToSeconds(it->readingMs)};
  }
  std::reverse(out, out + n);
  return n;
}

Result uploadGlobal(GfxRenderer& renderer, char* buffer, const char* deviceId) {
  const auto& stats = READING_STATS;
  const auto& days = stats.getReadingDays();
  StatsPayload::Global g;
  g.deviceId = deviceId;
  g.deviceName = DEVICE_NAME;
  g.sessions = stats.getTotalSessionCount();
  g.seconds = msToSeconds(stats.getTotalReadingMs());
  g.pages = stats.totalPagesRead;
  g.completed = stats.getBooksFinishedCount();
  const uint64_t* tod = stats.getTimeOfDayMs();
  for (size_t i = 0; i < 4; i++) g.tod[i] = msToSeconds(tod[i]);
  fillDow(days, g.dow);
  // Anchor the history on today when the clock is real, else on the newest
  // reading day; with neither there is no date to report.
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  g.anchorOrdinal = TimeUtils::isClockValid(now) ? TimeUtils::getLocalDayOrdinal(now) : 0;
  for (const auto& day : days) {
    if (day.readingMs > 0) g.anchorOrdinal = std::max(g.anchorOrdinal, day.dayOrdinal);
  }
  if (g.anchorOrdinal < StatsPayload::SERVER_EPOCH_OFFSET_DAYS) {
    LOG_INF("STATSYNC", "No dated reading yet; skipping the overall snapshot");
    return Result::Ok;
  }
  for (const auto& day : days) {
    if (day.readingMs > 0) StatsPayload::markHistory(g.history, g.anchorOrdinal, day.dayOrdinal);
  }
  g.streak = stats.getMaxStreakDays();
  StatsPayload::DayValue daily[StatsPayload::MAX_DAILY];
  g.dailyCount = newestDays(days, daily, StatsPayload::MAX_DAILY);
  g.daily = daily;
  if (!StatsPayload::global(buffer, PAYLOAD_CAPACITY, g)) {
    LOG_ERR("STATSYNC", "Overall snapshot does not fit");
    return Result::Failed;
  }
  return put(renderer, "/api/v1/stats/global", buffer);
}

Result uploadBooks(GfxRenderer& renderer, char* buffer, const char* deviceId, unsigned& booksSent) {
  StatsPayload::BookBatch batch(buffer, PAYLOAD_CAPACITY);
  if (!batch.begin(deviceId)) return Result::Failed;
  Result result = Result::Ok;
  size_t inBatch = 0;
  const auto send = [&]() -> bool {
    if (inBatch == 0) return true;
    if (!batch.finish()) return false;
    result = put(renderer, "/api/v1/stats/books", batch.payload());
    if (result != Result::Ok) return false;
    booksSent += static_cast<unsigned>(inBatch);
    inBatch = 0;
    return batch.begin(deviceId);
  };

  for (const auto& book : READING_STATS.getBooks()) {
    if (book.totalReadingMs == 0 || book.path.empty() || !Storage.exists(book.path.c_str())) continue;
    const std::string document = documentIdFor(book.path);
    if (document.size() != 32) continue;
    StatsPayload::Book item;
    item.document = document.c_str();
    item.sessions = book.sessions;
    item.seconds = msToSeconds(book.totalReadingMs);
    item.completed = book.completed;
    item.startDate = book.firstReadAt;
    item.finishedDate = book.completed ? book.completedAt : 0;
    fillDow(book.readingDays, item.dow);
    StatsPayload::DayValue daily[BOOK_DAILY];
    item.dailyCount = newestDays(book.readingDays, daily, BOOK_DAILY);
    item.daily = daily;
    if (!batch.add(item)) {
      // Full: send what we have, then retry this book in a fresh batch.
      if (!send()) return result == Result::Ok ? Result::Failed : result;
      if (!batch.add(item)) {
        LOG_ERR("STATSYNC", "Book snapshot does not fit: %s", book.path.c_str());
        continue;
      }
    }
    inBatch++;
  }
  if (!send()) return result == Result::Ok ? Result::Failed : result;
  return Result::Ok;
}
}  // namespace

Result upload(GfxRenderer& renderer, unsigned& booksSent) {
  booksSent = 0;
  if (!KOREADER_STORE.getSyncStats() || !KOREADER_STORE.supportsSyncExtensions()) return Result::Off;

  char deviceId[KOReaderSyncClient::READER_ID_SIZE];
  if (!KOReaderSyncClient::readerId(deviceId, sizeof(deviceId))) return Result::Failed;
  // 4KB: well past the 256-byte stack rule, and only alive for this step.
  std::unique_ptr<char, decltype(&free)> buffer(static_cast<char*>(malloc(PAYLOAD_CAPACITY)), &free);
  if (!buffer) {
    LOG_ERR("STATSYNC", "malloc failed: %u bytes", static_cast<unsigned>(PAYLOAD_CAPACITY));
    return Result::LowMemory;
  }

  Result result = uploadGlobal(renderer, buffer.get(), deviceId);
  if (result == Result::Ok) result = uploadBooks(renderer, buffer.get(), deviceId, booksSent);
  LOG_INF("STATSYNC", "Stats upload: result %d, %u books (heap %u)", static_cast<int>(result), booksSent,
          (unsigned)ESP.getFreeHeap());
  return result;
}

const char* label(const Result result) {
  switch (result) {
    case Result::Off:
      return tr(STR_STATE_OFF);
    case Result::Ok:
      return tr(STR_DONE);
    case Result::Unsupported:
      return tr(STR_SYNC_REQUIRES_CROSSPOINT);
    case Result::LowMemory:
      return tr(STR_DICT_LOW_MEMORY);
    case Result::Failed:
      return tr(STR_SYNC_FAILED_MSG);
  }
  return "";
}

}  // namespace StatsSync
