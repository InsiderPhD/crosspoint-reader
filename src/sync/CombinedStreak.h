#pragma once

#include <cstdint>

// The reading streak across every reader on the CrossPoint Sync account, read
// back from GET /api/v1/stats/summary by Sync Books. Kept apart from the
// local streak on purpose: READING_STATS only ever counts this reader (its
// streak is goal-based), the combined one is the server's "any reading that
// day" streak over all devices. The Stats screen shows both.
//
// Stored in /.crosspoint/kosync_streak.json; 12 bytes of .bss once read.
namespace CombinedStreak {

struct Value {
  bool valid = false;
  uint32_t current = 0;        // server current_streak, as of anchorOrdinal
  uint32_t longest = 0;        // server streak (longest)
  uint32_t anchorOrdinal = 0;  // local day ordinal (days since 1970) the streak ends on
};

// Cached after the first call; cheap to call from render().
const Value& get();
// Current streak as of today: 0 once a full day has passed since the anchor
// with no reading reported (same rule as the local streak).
uint32_t currentAsOf(uint32_t todayOrdinal);
// Write a fresh summary (and refresh the cache). No card write when unchanged.
bool save(uint32_t current, uint32_t longest, uint32_t anchorOrdinal);

}  // namespace CombinedStreak
