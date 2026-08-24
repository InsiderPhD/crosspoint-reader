#pragma once

#include <cstdint>

// Brightness rungs offered by FrontlightBrightnessActivity, the picker behind the
// Settings > Display and reader-menu frontlight rows.
//
// The ramp is deliberately uneven. The SDK maps percent to duty through a
// gamma-1.6554 curve, so on the X4 Pro's 10-bit PWM the bottom of the scale is
// where the useful resolution lives: 1% is duty 1 of 1023, 5% is 7, 10% is 23.
// Dense rungs down there give a genuinely dark-room-usable light; above ~30% a
// 10-point step is barely a visible change, so the top half stays coarse.
//
// The stored setting is still a plain 0-100 percentage — the web UI's slider
// steps in 1% and can set values off this ladder. nearestIndex() snaps those to
// the closest rung when the picker opens, and leaves the stored value alone
// until the user actually chooses something.
namespace FrontlightLevels {

inline constexpr uint8_t LEVELS[] = {0, 1, 2, 3, 5, 7, 10, 15, 20, 30, 40, 50, 60, 70, 80, 90, 100};
inline constexpr int COUNT = static_cast<int>(sizeof(LEVELS) / sizeof(LEVELS[0]));

// Rung closest to `percent`, for seeding the picker's cursor from a stored value.
inline int nearestIndex(uint8_t percent) {
  int best = 0;
  int bestDistance = 255;
  for (int i = 0; i < COUNT; ++i) {
    const int distance = (LEVELS[i] > percent) ? (LEVELS[i] - percent) : (percent - LEVELS[i]);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

}  // namespace FrontlightLevels
