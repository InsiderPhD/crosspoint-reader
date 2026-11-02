// Host test for src/sync/StatsPayload: dates, weekdays, the history bitmap's
// bit order and the JSON the CrossPoint Sync stats routes receive.
#include <cstdio>
#include <cstring>
#include <string>

#include "StatsPayload.h"

namespace {
int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    failures++;
  }
}

bool contains(const char* haystack, const char* needle) { return std::strstr(haystack, needle) != nullptr; }
}  // namespace

int main() {
  using namespace StatsPayload;

  // 2026-11-02 is ordinal 20759 (days since 1970-01-01) and a Monday.
  int y = 0;
  unsigned m = 0, d = 0;
  civilFromOrdinal(20759, y, m, d);
  check(y == 2026 && m == 11 && d == 2, "civil date of 2026-11-02");
  civilFromOrdinal(0, y, m, d);
  check(y == 1970 && m == 1 && d == 1, "civil date of the epoch");
  civilFromOrdinal(19417, y, m, d);  // 2023-03-01, just after a non-leap February
  check(y == 2023 && m == 3 && d == 1, "civil date across February");
  check(mondayIndex(20759) == 0, "2026-11-02 is a Monday");
  check(mondayIndex(0) == 3, "1970-01-01 is a Thursday");
  check(mondayIndex(20758) == 6, "2026-11-01 is a Sunday");

  // Bit N = anchor - N, LSB first within each byte.
  uint8_t history[HISTORY_BYTES] = {};
  markHistory(history, 20759, 20759);        // bit 0
  markHistory(history, 20759, 20750);        // bit 9 -> byte 1, bit 1
  markHistory(history, 20759, 20760);        // after the anchor: ignored
  markHistory(history, 20759, 20759 - 730);  // outside the window: ignored
  check(history[0] == 0x01 && history[1] == 0x02, "history bit order");
  int set = 0;
  for (const uint8_t b : history) set += __builtin_popcount(b);
  check(set == 2, "history ignores out-of-window days");

  char out[2048];
  const DayValue days[] = {{20758, 600}, {20759, 1200}};
  Global g;
  g.deviceId = "katiepoint-0123456789abcdef";
  g.deviceName = "KatiePoint";
  g.sessions = 3;
  g.seconds = 1800;
  g.pages = 42;
  g.completed = 1;
  g.tod[0] = 1800;
  g.dow[0] = 1200;
  g.dow[6] = 600;
  g.anchorOrdinal = 20759;
  memcpy(g.history, history, sizeof(history));
  g.streak = 2;
  g.daily = days;
  g.dailyCount = 2;
  check(global(out, sizeof(out), g), "global payload fits");
  check(contains(out, "\"anchor_day\":9802,"), "anchor_day counts from 2000-01-01");
  check(contains(out, "\"tod\":[1800,0,0,0]"), "tod array");
  check(contains(out, "\"dow\":[1200,0,0,0,0,0,600]"), "dow array");
  check(contains(out, "\"history_b64\":\"AQI"), "history base64 starts with bytes 01 02");
  check(contains(out, "{\"date\":\"2026-11-01\",\"seconds\":600},{\"date\":\"2026-11-02\",\"seconds\":1200}"),
        "daily entries");
  // 92 bytes -> 124 base64 characters ending in one '='.
  const char* b64 = std::strstr(out, "\"history_b64\":\"") + 15;
  const char* end = std::strchr(b64, '"');
  check(end - b64 == 124 && end[-1] == '=' && end[-2] != '=', "history base64 length and padding");
  check(!global(out, 64, g), "global payload refuses a small buffer");
  g.deviceId = "bad id";
  check(!global(out, sizeof(out), g), "global payload refuses an unsafe device id");

  BookBatch batch(out, 900);
  check(batch.begin("katiepoint-0123456789abcdef"), "book batch begins");
  Book b;
  b.document = "0123456789abcdef0123456789abcdef";
  b.sessions = 2;
  b.seconds = 1200;
  b.completed = true;
  b.startDate = 1793600000;
  b.daily = days;
  b.dailyCount = 2;
  check(batch.add(b), "first book fits");
  check(batch.add(b), "second book fits");
  size_t added = batch.count();
  while (batch.add(b)) added++;
  check(added == batch.count() && added >= 2, "a full batch refuses further books");
  check(batch.finish(), "book batch closes");
  const std::string json(out);
  check(json.rfind("]}") == json.size() - 2, "book batch ends with ]}");
  check(contains(out, "\"completed\":true"), "book completed flag");
  Book bad = b;
  bad.document = "NOT-A-HASH";
  BookBatch other(out, sizeof(out));
  check(other.begin("katiepoint-0123456789abcdef") && !other.add(bad), "book batch refuses a bad document id");

  if (failures == 0) std::printf("stats_payload: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
