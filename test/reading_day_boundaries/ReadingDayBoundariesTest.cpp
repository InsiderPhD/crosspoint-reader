// Host test for the reading-stats day boundary and streak maths across
// daylight-saving changes. Plain checks, no framework: see
// test/run_reading_day_boundaries_test.sh.
//
// What is under test is the firmware's own code: TimeUtils::getLocalDayOrdinal /
// getLocalHour (src/util/TimeUtils.cpp), the zone table and POSIX rules in
// src/util/TimeZoneRegistry.cpp, and ReadingStreaks::compute (the streak walk
// ReadingStatsStore::rebuildSummaryCache runs). Only the settings singleton and
// HalClock are host stand-ins.
//
// The property that matters: a reading day is a LOCAL CALENDAR day, so the
// 23-hour day when the clocks go forward and the 25-hour day when they go back
// are each exactly one day, and a streak carries straight through both.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "ReadingStreaks.h"
#include "TimeUtils.h"
#include "TimeZoneRegistry.h"

namespace {
int failures = 0;

void expectTrue(const bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL %s\n", what);
  failures++;
}

void expectEq(const uint32_t got, const uint32_t want, const char* what) {
  if (got == want) return;
  std::printf("FAIL %s: got %u want %u\n", what, static_cast<unsigned>(got), static_cast<unsigned>(want));
  failures++;
}

// UTC wall time -> epoch seconds, independent of the TZ under test.
uint32_t utc(const int year, const unsigned month, const unsigned day, const unsigned hour, const unsigned minute) {
  return TimeUtils::getDayOrdinalForDate(year, month, day) * 86400u + hour * 3600u + minute * 60u;
}

uint32_t dayOf(const int year, const unsigned month, const unsigned day) {
  return TimeUtils::getDayOrdinalForDate(year, month, day);
}

// Select a zone the way the picker does: by its table index.
bool selectZone(const char* label, const uint8_t dstMode = TimeZoneRegistry::DST_AUTO) {
  for (size_t i = 0; i < TimeZoneRegistry::getPresetCount(); i++) {
    if (std::strcmp(TimeZoneRegistry::getPreset(static_cast<uint8_t>(i)).label, label) == 0) {
      SETTINGS.clockTimezone = static_cast<uint8_t>(i);
      SETTINGS.clockDst = dstMode;
      return true;
    }
  }
  std::printf("FAIL zone \"%s\" is not in the table\n", label);
  failures++;
  return false;
}

uint8_t hourOf(const uint32_t epoch) {
  uint8_t hour = 255;
  TimeUtils::getLocalHour(epoch, hour);
  return hour;
}

void testLondon() {
  if (!selectZone("London / Dublin / Lisbon")) return;

  // Clocks go forward 2026-03-29 01:00 UTC: that local day is 23 hours long
  // (00:00 UTC to 23:00 UTC).
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 28, 23, 30)), dayOf(2026, 3, 28),
           "London: 23:30 GMT on the 28th");
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 29, 0, 10)), dayOf(2026, 3, 29),
           "London: 00:10 on the short day");
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 29, 22, 50)), dayOf(2026, 3, 29),
           "London: 23:50 BST on the short day");
  // 23:30 UTC is already 00:30 BST on the 30th. A fixed standard offset (the
  // model this clock stack replaced) would file it under the 29th.
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 29, 23, 30)), dayOf(2026, 3, 30),
           "London: 00:30 BST belongs to the next day");

  // Clocks go back 2026-10-25 01:00 UTC: that local day is 25 hours long
  // (23:00 UTC on the 24th to 24:00 UTC on the 25th).
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 10, 24, 23, 10)), dayOf(2026, 10, 25),
           "London: 00:10 BST on the long day");
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 10, 25, 23, 50)), dayOf(2026, 10, 25),
           "London: 23:50 GMT on the long day");
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 10, 26, 0, 10)), dayOf(2026, 10, 26),
           "London: day after the long day");

  // Time-of-day buckets follow the same rule.
  expectEq(hourOf(utc(2026, 6, 1, 12, 0)), 13, "London: summer hour");
  expectEq(hourOf(utc(2026, 1, 5, 12, 0)), 12, "London: winter hour");

  // The DST override pins a fixed offset all year.
  selectZone("London / Dublin / Lisbon", TimeZoneRegistry::DST_OFF);
  expectEq(hourOf(utc(2026, 6, 1, 12, 0)), 12, "London, DST off: summer hour");
  selectZone("London / Dublin / Lisbon", TimeZoneRegistry::DST_ON);
  expectEq(hourOf(utc(2026, 1, 5, 12, 0)), 13, "London, DST on: winter hour");
  expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 1, 5, 23, 30)), dayOf(2026, 1, 6),
           "London, DST on: 00:30 belongs to the next day");
}

void testOtherHemispheres() {
  if (selectZone("New York / Toronto")) {
    // Forward 2026-03-08 07:00 UTC; back 2026-11-01 06:00 UTC.
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 8, 5, 10)), dayOf(2026, 3, 8), "New York: 00:10 EST short day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 9, 3, 50)), dayOf(2026, 3, 8), "New York: 23:50 EDT short day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 3, 9, 4, 5)), dayOf(2026, 3, 9), "New York: 00:05 EDT next day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 11, 1, 4, 10)), dayOf(2026, 11, 1),
             "New York: 00:10 EDT long day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 11, 2, 4, 50)), dayOf(2026, 11, 1),
             "New York: 23:50 EST long day");
  }
  if (selectZone("Sydney / Melbourne")) {
    // Back 2026-04-04 16:00 UTC (03:00 AEDT on the 5th): a 25-hour 5 April.
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 4, 4, 13, 10)), dayOf(2026, 4, 5), "Sydney: 00:10 AEDT long day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 4, 5, 13, 50)), dayOf(2026, 4, 5), "Sydney: 23:50 AEST long day");
    expectEq(TimeUtils::getLocalDayOrdinal(utc(2026, 4, 5, 14, 10)), dayOf(2026, 4, 6),
             "Sydney: day after the long day");
  }
}

// Reading at 21:00 local every evening for a week that contains the change must
// be seven consecutive days and one seven-day streak, in both directions.
void testStreakAcrossChange(const char* label, const int year, const unsigned month, const unsigned firstDay,
                            const int utcHourBefore, const int utcHourAfter, const unsigned changeDay) {
  if (!selectZone(label)) return;
  uint32_t days[7];
  for (unsigned i = 0; i < 7; i++) {
    const unsigned day = firstDay + i;
    // 21:00 local is a different UTC hour on either side of the change.
    const int utcHour = day < changeDay ? utcHourBefore : utcHourAfter;
    days[i] = TimeUtils::getLocalDayOrdinal(utc(year, month, day, static_cast<unsigned>(utcHour), 0));
    expectEq(days[i], dayOf(year, month, day), "evening read lands on its own calendar day");
  }
  const ReadingStreaks::Result streak = ReadingStreaks::compute(days, 7, days[6]);
  expectEq(streak.currentDays, 7, label);
  expectEq(streak.maxDays, 7, label);
}

// Every zone in the table, every quarter hour of 2026: the local day never goes
// backwards, never skips, lasts 23-25 hours, and the year has 365 of them.
void testEveryZoneAllYear() {
  const uint32_t start = utc(2026, 1, 1, 12, 0);
  const uint32_t end = utc(2027, 1, 1, 12, 0);
  for (size_t i = 0; i < TimeZoneRegistry::getPresetCount(); i++) {
    SETTINGS.clockTimezone = static_cast<uint8_t>(i);
    SETTINGS.clockDst = TimeZoneRegistry::DST_AUTO;
    const char* label = TimeZoneRegistry::getPreset(static_cast<uint8_t>(i)).label;

    uint32_t previous = TimeUtils::getLocalDayOrdinal(start);
    uint32_t dayStart = 0;  // unknown for the partial first day
    uint32_t changes = 0;
    bool ok = true;
    for (uint32_t t = start + 900; t <= end && ok; t += 900) {
      const uint32_t ordinal = TimeUtils::getLocalDayOrdinal(t);
      if (ordinal == previous) continue;
      if (ordinal != previous + 1) {
        std::printf("FAIL %s: day went from %u to %u\n", label, static_cast<unsigned>(previous),
                    static_cast<unsigned>(ordinal));
        ok = false;
      }
      if (dayStart != 0) {
        const uint32_t length = t - dayStart;
        if (length < 23u * 3600u || length > 25u * 3600u) {
          std::printf("FAIL %s: a day lasted %u seconds\n", label, static_cast<unsigned>(length));
          ok = false;
        }
      }
      dayStart = t;
      previous = ordinal;
      changes++;
    }
    if (ok && changes != 365) {
      std::printf("FAIL %s: 2026 had %u day changes\n", label, static_cast<unsigned>(changes));
      ok = false;
    }
    if (!ok) failures++;
  }
}

void testStreakArithmetic() {
  const uint32_t run[] = {100, 101, 102, 104, 105};
  ReadingStreaks::Result r = ReadingStreaks::compute(run, 5, 105);
  expectEq(r.currentDays, 2, "streak: current run ends today");
  expectEq(r.maxDays, 3, "streak: longest run");

  r = ReadingStreaks::compute(run, 5, 106);
  expectEq(r.currentDays, 2, "streak: still alive when the last day was yesterday");

  r = ReadingStreaks::compute(run, 5, 107);
  expectEq(r.currentDays, 0, "streak: over after a missed day");
  expectEq(r.maxDays, 3, "streak: longest run survives a broken streak");

  r = ReadingStreaks::compute(nullptr, 0, 105);
  expectEq(r.currentDays, 0, "streak: no days");
  expectEq(r.maxDays, 0, "streak: no days, no max");

  const uint32_t one[] = {105};
  r = ReadingStreaks::compute(one, 1, 105);
  expectEq(r.currentDays, 1, "streak: single day");
  expectEq(r.maxDays, 1, "streak: single day max");
}
}  // namespace

int main() {
  expectTrue(TimeUtils::getLocalDayOrdinal(0) == 0, "an unset clock has no day");

  testLondon();
  testOtherHemispheres();
  // London: 21:00 local is 21:00 UTC before 29 March and 20:00 UTC from it.
  testStreakAcrossChange("London / Dublin / Lisbon", 2026, 3, 26, 21, 20, 29);
  // ...and 20:00 UTC before 25 October, 21:00 UTC from it.
  testStreakAcrossChange("London / Dublin / Lisbon", 2026, 10, 22, 20, 21, 25);
  // Berlin: 21:00 local is 20:00 UTC in winter, 19:00 UTC in summer.
  testStreakAcrossChange("Berlin / Paris / Madrid / Rome", 2026, 3, 26, 20, 19, 29);
  testStreakAcrossChange("Berlin / Paris / Madrid / Rome", 2026, 10, 22, 19, 20, 25);
  testEveryZoneAllYear();
  testStreakArithmetic();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("reading day boundary tests passed\n");
  return 0;
}
