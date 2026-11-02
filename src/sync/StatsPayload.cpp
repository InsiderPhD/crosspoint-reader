#include "StatsPayload.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace StatsPayload {
namespace {
class Writer {
 public:
  Writer(char* out, size_t capacity) : out_(out), remaining_(capacity) {}
  void add(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    if (!ok_) return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out_, remaining_, format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= remaining_) {
      ok_ = false;
      return;
    }
    out_ += n;
    remaining_ -= static_cast<size_t>(n);
  }
  void array(const char* key, const uint32_t* values, size_t count) {
    add(",\"%s\":[", key);
    for (size_t i = 0; i < count; ++i) add("%s%lu", i ? "," : "", static_cast<unsigned long>(values[i]));
    add("]");
  }
  void daily(const DayValue* days, size_t count) {
    if (!days || count == 0) return;
    if (count > MAX_DAILY) {
      days += count - MAX_DAILY;  // the newest days
      count = MAX_DAILY;
    }
    add(",\"daily\":[");
    for (size_t i = 0; i < count; ++i) {
      int y = 0;
      unsigned m = 0, d = 0;
      civilFromOrdinal(days[i].ordinal, y, m, d);
      add("%s{\"date\":\"%04d-%02u-%02u\",\"seconds\":%lu}", i ? "," : "", y, m, d,
          static_cast<unsigned long>(days[i].seconds));
    }
    add("]");
  }
  bool ok() const { return ok_; }

 private:
  char* out_;
  size_t remaining_;
  bool ok_ = true;
};

bool validDocument(const char* document) {
  if (!document || strlen(document) != 32) return false;
  for (const char* c = document; *c; ++c) {
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
  }
  return true;
}

// Append one item to a {"device_id":...,"items":[ ... ]} batch, rolling back
// on overflow. `write` fills the item.
template <typename Fn>
bool appendItem(char* out, const size_t capacity, size_t& length, size_t& items, Fn&& write) {
  if (length == 0 || capacity - length < 3) return false;
  Writer w(out + length, capacity - length - 2);  // keep room for "]}"
  if (items) w.add(",");
  write(w);
  if (!w.ok()) {
    out[length] = '\0';
    return false;
  }
  length += strlen(out + length);
  ++items;
  return true;
}

bool beginBatch(char* out, const size_t capacity, size_t& length, size_t& items, const char* deviceId) {
  length = 0;
  items = 0;
  if (!out || capacity == 0 || !validId(deviceId, 64)) return false;
  Writer w(out, capacity);
  w.add("{\"device_id\":\"%s\",\"items\":[", deviceId);
  if (!w.ok()) return false;
  length = strlen(out);
  return true;
}

bool finishBatch(char* out, const size_t capacity, size_t& length, const size_t items) {
  if (length == 0 || items == 0 || capacity - length < 3) return false;
  memcpy(out + length, "]}", 3);
  length += 2;
  return true;
}
}  // namespace

void civilFromOrdinal(const uint32_t ordinal, int& year, unsigned& month, unsigned& day) {
  // Howard Hinnant's civil_from_days.
  const int64_t z = static_cast<int64_t>(ordinal) + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp < 10 ? mp + 3 : mp - 9;
  year = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

uint8_t mondayIndex(const uint32_t ordinal) {
  // 1970-01-01 was a Thursday (Monday-based index 3).
  return static_cast<uint8_t>((ordinal + 3) % 7);
}

void markHistory(uint8_t (&history)[HISTORY_BYTES], const uint32_t anchorOrdinal, const uint32_t ordinal) {
  if (ordinal > anchorOrdinal) return;
  const uint32_t n = anchorOrdinal - ordinal;
  if (n >= HISTORY_DAYS) return;
  history[n / 8] |= static_cast<uint8_t>(1u << (n % 8));
}

bool validId(const char* id, const size_t maxLength) {
  if (!id || !*id || strlen(id) > maxLength) return false;
  for (; *id; ++id) {
    const char c = *id;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}

bool global(char* out, const size_t capacity, const Global& g) {
  if (!out || capacity == 0 || !validId(g.deviceId, 64) || g.anchorOrdinal < SERVER_EPOCH_OFFSET_DAYS) return false;
  Writer w(out, capacity);
  // The display name is ours (a constant), never user text, so it needs no escaping.
  w.add(
      "{\"device_id\":\"%s\",\"device\":\"%s\",\"v\":5,\"sessions\":%lu,\"seconds\":%lu,\"pages\":%lu,\"completed\":%"
      "lu",
      g.deviceId, g.deviceName, static_cast<unsigned long>(g.sessions), static_cast<unsigned long>(g.seconds),
      static_cast<unsigned long>(g.pages), static_cast<unsigned long>(g.completed));
  w.array("tod", g.tod, 4);
  w.array("dow", g.dow, 7);
  w.add(",\"anchor_day\":%lu,\"streak\":%lu,\"history_b64\":\"",
        static_cast<unsigned long>(g.anchorOrdinal - SERVER_EPOCH_OFFSET_DAYS), static_cast<unsigned long>(g.streak));
  static constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (size_t i = 0; i < HISTORY_BYTES; i += 3) {
    const uint32_t value = (static_cast<uint32_t>(g.history[i]) << 16) |
                           (i + 1 < HISTORY_BYTES ? static_cast<uint32_t>(g.history[i + 1]) << 8 : 0) |
                           (i + 2 < HISTORY_BYTES ? g.history[i + 2] : 0);
    w.add("%c%c%c%c", BASE64[(value >> 18) & 63], BASE64[(value >> 12) & 63],
          i + 1 < HISTORY_BYTES ? BASE64[(value >> 6) & 63] : '=', i + 2 < HISTORY_BYTES ? BASE64[value & 63] : '=');
  }
  w.add("\"");
  w.daily(g.daily, g.dailyCount);
  w.add("}");
  return w.ok();
}

bool BookBatch::begin(const char* deviceId) { return beginBatch(out_, capacity_, length_, items_, deviceId); }

bool BookBatch::add(const Book& b) {
  if (items_ >= MAX_BOOK_BATCH || !validDocument(b.document)) return false;
  return appendItem(out_, capacity_, length_, items_, [&b](Writer& w) {
    w.add(
        "{\"document\":\"%s\",\"v\":5,\"sessions\":%lu,\"seconds\":%lu,\"pages\":0,\"completed\":%s,\"avg_fwd\":0,"
        "\"pace_n\":0,\"eta\":0,\"start_manual\":false,\"finish_manual\":false,\"start_date\":%lu,"
        "\"finished_date\":%lu",
        b.document, static_cast<unsigned long>(b.sessions), static_cast<unsigned long>(b.seconds),
        b.completed ? "true" : "false", static_cast<unsigned long>(b.startDate),
        static_cast<unsigned long>(b.finishedDate));
    static constexpr uint32_t NO_TOD[4] = {};  // not tracked per book
    w.array("tod", NO_TOD, 4);
    w.array("dow", b.dow, 7);
    w.daily(b.daily, b.dailyCount);
    w.add("}");
  });
}

bool BookBatch::finish() { return finishBatch(out_, capacity_, length_, items_); }

}  // namespace StatsPayload
