#pragma once

#include <cstddef>
#include <cstdint>

// Streak arithmetic for ReadingStatsStore, kept free of the store, the clock
// and the SD card so it can be exercised on the host (test/reading_day_boundaries).
//
// Everything here works on LOCAL CALENDAR DAY ordinals (TimeUtils::
// getLocalDayOrdinal), never on elapsed seconds. That is what keeps a streak
// intact across a daylight-saving change: the 23-hour and 25-hour days are each
// exactly one ordinal, so "consecutive" stays "+1" whatever the clocks did.
namespace ReadingStreaks {

struct Result {
  uint32_t currentDays = 0;
  uint32_t maxDays = 0;
};

// `eligibleDays` are the day ordinals that met the daily goal, ascending and
// without duplicates. `referenceDay` is today's ordinal. The current streak is
// alive while its last day is today or yesterday (today's reading may simply
// not have happened yet); otherwise it is 0.
inline Result compute(const uint32_t* eligibleDays, const size_t count, const uint32_t referenceDay) {
  Result result;
  if (eligibleDays == nullptr || count == 0) {
    return result;
  }

  result.maxDays = 1;
  uint32_t run = 1;
  for (size_t index = 1; index < count; ++index) {
    run = (eligibleDays[index] == eligibleDays[index - 1] + 1) ? run + 1 : 1;
    if (run > result.maxDays) {
      result.maxDays = run;
    }
  }

  const uint32_t latest = eligibleDays[count - 1];
  const bool alive = latest == referenceDay || (referenceDay > 0 && latest + 1 == referenceDay);
  if (alive) {
    result.currentDays = 1;
    for (size_t index = count - 1; index > 0; --index) {
      if (eligibleDays[index] != eligibleDays[index - 1] + 1) {
        break;
      }
      result.currentDays++;
    }
  }
  return result;
}

}  // namespace ReadingStreaks
