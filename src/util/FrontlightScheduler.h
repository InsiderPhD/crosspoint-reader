#pragma once

#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"

// Frontlight schedules ("night shift"). Each enabled slot in
// SETTINGS.frontlightSchedules names a daily window and the brightness/warmth
// the light should hold inside it. The scheduler is edge-triggered: it only
// acts when the matching slot changes.
//
//   none -> slot   : capture the current brightness/warmth as the restore
//                    point, then switch to the slot's level.
//   slot -> other  : keep the original restore point, switch to the new level.
//   slot -> none   : put the restore point back.
//
// Between edges the light is left alone, so a manual change inside a window
// (reader menu, Settings, the Light action) sticks until the window ends. The
// slot's level is written to SETTINGS.frontlightBrightness/Warmth like any
// other setter, so every screen shows the level that is actually lit.
//
// Everything is a no-op on boards without a frontlight and whenever the system
// clock is not trustworthy (no NTP yet and no RTC seed).
namespace FrontlightScheduler {

// True when `minuteOfDay` (0-1439, local) is inside the slot's window. A window
// whose end is at or before its start wraps past midnight; start == end is
// treated as the full day.
bool slotContains(const CrossPointSettings::FrontlightSchedule& slot, uint16_t minuteOfDay);

// Index of the first enabled slot whose window contains the minute, or -1.
// Earlier slots win when windows overlap.
int matchingSlot(uint16_t minuteOfDay);

// Compare the schedules against the local clock and drive the light on a
// transition. Saves settings when it changes them. Returns true when the light
// was changed. Call at boot (after settings load and halFrontlight.begin())
// and after editing a schedule.
bool evaluate();

// evaluate() throttled to once a minute of wall time, for the main loop.
void tick();

// "21:00" or, with the 12-hour clock format, "9:00 PM". buf needs >= 9 bytes.
void formatMinuteOfDay(uint16_t minuteOfDay, char* buf, size_t bufSize);

}  // namespace FrontlightScheduler
