#pragma once

#include <cstddef>
#include <cstdint>

// JSON bodies for the CrossPoint Sync stats extension (crosspoint-sync
// docs/API.md: PUT /api/v1/stats/global and /stats/books),
// written straight into a caller's fixed buffer: no JSON tree, no heap.
// Free of Arduino and the stats store so it builds in the host test
// (test/stats_payload).
//
// Day ordinals are this firmware's local calendar days since 1970-01-01
// (TimeUtils::getLocalDayOrdinal); the server counts days since 2000-01-01.
namespace StatsPayload {

constexpr uint32_t SERVER_EPOCH_OFFSET_DAYS = 10957;  // 1970-01-01 -> 2000-01-01
constexpr size_t HISTORY_BYTES = 92;                  // 730 days, LSB-first per byte
constexpr size_t HISTORY_DAYS = 730;
constexpr size_t MAX_DAILY = 20;       // server cap per item
constexpr size_t MAX_BOOK_BATCH = 20;  // server cap per request

struct DayValue {
  uint32_t ordinal = 0;  // local day, days since 1970-01-01
  uint32_t seconds = 0;
};

struct Global {
  const char* deviceId = "";
  const char* deviceName = "";
  uint32_t sessions = 0;
  uint32_t seconds = 0;
  uint32_t pages = 0;
  uint32_t completed = 0;
  uint32_t tod[4] = {};  // morning, afternoon, evening, night
  uint32_t dow[7] = {};  // Monday .. Sunday
  uint32_t anchorOrdinal = 0;
  uint8_t history[HISTORY_BYTES] = {};
  uint32_t streak = 0;
  const DayValue* daily = nullptr;
  size_t dailyCount = 0;
};

struct Book {
  const char* document = "";  // 32 lowercase hex
  uint32_t sessions = 0;
  uint32_t seconds = 0;
  bool completed = false;
  uint32_t startDate = 0;     // unix seconds, 0 = unknown
  uint32_t finishedDate = 0;  // unix seconds, 0 = not finished
  uint32_t dow[7] = {};
  const DayValue* daily = nullptr;
  size_t dailyCount = 0;
};

constexpr size_t MAX_SESSION_BATCH = 20;  // server cap per request

struct Session {
  const char* id = "";        // 1-64 of [A-Za-z0-9_-]
  const char* document = "";  // 32 lowercase hex
  uint32_t startedAt = 0;     // unix UTC seconds
  uint32_t endedAt = 0;
  uint32_t seconds = 0;  // active seconds: >= 10 and <= the wall span
  int32_t utcOffsetMinutes = 0;
};

// Civil date of a day ordinal (days since 1970-01-01).
void civilFromOrdinal(uint32_t ordinal, int& year, unsigned& month, unsigned& day);
// 0 = Monday .. 6 = Sunday.
uint8_t mondayIndex(uint32_t ordinal);
// Marks `ordinal` in a history bitmap anchored at `anchorOrdinal` (bit N =
// anchor - N). Days after the anchor or older than the window are ignored.
void markHistory(uint8_t (&history)[HISTORY_BYTES], uint32_t anchorOrdinal, uint32_t ordinal);
// True for 1..64 characters of [A-Za-z0-9_-]: safe to embed without escaping.
bool validId(const char* id, size_t maxLength);

bool global(char* out, size_t capacity, const Global& g);

// Several books in one request. add() leaves the batch unchanged and returns
// false when the item does not fit, so the caller sends and starts again.
class BookBatch {
 public:
  BookBatch(char* out, size_t capacity) : out_(out), capacity_(capacity) {}
  bool begin(const char* deviceId);
  bool add(const Book& book);
  bool finish();
  size_t count() const { return items_; }
  const char* payload() const { return out_; }

 private:
  char* out_;
  size_t capacity_;
  size_t length_ = 0;
  size_t items_ = 0;
};

// Same shape for the session journal (PUT /api/v1/stats/sessions). add()
// also refuses a session the server would reject (< 10 s, or more active
// seconds than its wall span).
class SessionBatch {
 public:
  SessionBatch(char* out, size_t capacity) : out_(out), capacity_(capacity) {}
  bool begin(const char* deviceId);
  bool add(const Session& session);
  bool finish();
  size_t count() const { return items_; }
  const char* payload() const { return out_; }

 private:
  char* out_;
  size_t capacity_;
  size_t length_ = 0;
  size_t items_ = 0;
};

}  // namespace StatsPayload
