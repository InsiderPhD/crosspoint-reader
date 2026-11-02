#include "FrontlightScheduler.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <Logging.h>

#include <cstdio>

#include "TimeUtils.h"

namespace {
// The windows are minute-granular, so checking more often than this only
// costs a localtime_r for nothing.
constexpr unsigned long TICK_INTERVAL_MS = 30UL * 1000UL;
}  // namespace

bool FrontlightScheduler::slotContains(const CrossPointSettings::FrontlightSchedule& slot, const uint16_t minuteOfDay) {
  if (slot.startMinutes < slot.endMinutes) {
    return minuteOfDay >= slot.startMinutes && minuteOfDay < slot.endMinutes;
  }
  // Wraps past midnight (or start == end: the whole day).
  return minuteOfDay >= slot.startMinutes || minuteOfDay < slot.endMinutes;
}

void FrontlightScheduler::setDarkMode(const uint8_t on) {
  if (SETTINGS.darkMode == on) return;
  SETTINGS.darkMode = on;
  // Every pixel flips polarity; a differential fast refresh would ghost badly.
  // The caller's next repaint picks this up.
  display.requestResync(1);
}

int FrontlightScheduler::matchingSlot(const uint16_t minuteOfDay) {
  for (int i = 0; i < CrossPointSettings::FRONTLIGHT_SCHEDULE_SLOTS; i++) {
    const auto& slot = SETTINGS.frontlightSchedules[i];
    if (slot.enabled && slotContains(slot, minuteOfDay)) return i;
  }
  return -1;
}

bool FrontlightScheduler::controlsLight() {
#if FREEINK_CAP_FRONTLIGHT
  return halFrontlight.present();
#else
  return false;
#endif
}

bool FrontlightScheduler::evaluate() {
  uint16_t minuteOfDay = 0;
  if (!TimeUtils::getLocalMinuteOfDay(minuteOfDay)) {
    // No trustworthy clock yet (boot before NTP). Leave the light as the
    // saved settings put it; tick() retries once the clock is set.
    return false;
  }

  const int wanted = matchingSlot(minuteOfDay);
  const int active = static_cast<int>(SETTINGS.frontlightScheduleActive) - 1;
  if (wanted == active) return false;
  // Without a light only dark mode is scheduled; the brightness/warmth fields
  // are left exactly as the file had them so they round-trip to a lit board.
  const bool light = controlsLight();
  // Dark mode is only touched by slots that ask for it: a slot without it
  // leaves the user's setting alone, and only leaving a dark slot restores.
  const bool leavingDarkSlot = active >= 0 && SETTINGS.frontlightSchedules[active].darkMode;

  if (wanted < 0) {
    // Window over: back to the level captured when it began.
    if (light) {
      SETTINGS.frontlightBrightness = SETTINGS.frontlightScheduleRestoreBrightness;
      SETTINGS.frontlightWarmth = SETTINGS.frontlightScheduleRestoreWarmth;
    }
    if (leavingDarkSlot) setDarkMode(SETTINGS.frontlightScheduleRestoreDarkMode);
    SETTINGS.frontlightScheduleActive = 0;
    LOG_INF("FLSCHED", "Schedule %d ended at %02u:%02u; light back to %u%% / warmth %u%%, dark %u", active + 1,
            minuteOfDay / 60, minuteOfDay % 60, SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth,
            SETTINGS.darkMode);
  } else {
    const auto& slot = SETTINGS.frontlightSchedules[wanted];
    if (active < 0) {
      // Only the first window of a run captures the restore point; a hop
      // straight into another slot keeps the pre-schedule level.
      if (light) {
        SETTINGS.frontlightScheduleRestoreBrightness = SETTINGS.frontlightBrightness;
        SETTINGS.frontlightScheduleRestoreWarmth = SETTINGS.frontlightWarmth;
      }
      SETTINGS.frontlightScheduleRestoreDarkMode = SETTINGS.darkMode;
    }
    if (light) {
      SETTINGS.frontlightBrightness = slot.brightness;
      SETTINGS.frontlightWarmth = slot.warmth;
    }
    if (slot.darkMode) {
      setDarkMode(1);
    } else if (leavingDarkSlot) {
      setDarkMode(SETTINGS.frontlightScheduleRestoreDarkMode);
    }
    SETTINGS.frontlightScheduleActive = static_cast<uint8_t>(wanted + 1);
    LOG_INF("FLSCHED", "Schedule %d active at %02u:%02u; light to %u%% / warmth %u%%, dark %u", wanted + 1,
            minuteOfDay / 60, minuteOfDay % 60, SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth,
            SETTINGS.darkMode);
  }

  if (light) halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
  // A handful of writes a day at most: transitions are bounded by the number
  // of slots, so this does not fall under the interaction-write throttle.
  SETTINGS.saveToFile();
  return true;
}

void FrontlightScheduler::tick() {
  static unsigned long lastTickMs = 0;
  const unsigned long now = millis();
  if (lastTickMs != 0 && now - lastTickMs < TICK_INTERVAL_MS) return;
  lastTickMs = now;
  evaluate();
}

void FrontlightScheduler::formatMinuteOfDay(const uint16_t minuteOfDay, char* buf, const size_t bufSize) {
  const unsigned hour = minuteOfDay / 60;
  const unsigned minute = minuteOfDay % 60;
  if (SETTINGS.clockFormat == 1) {
    const unsigned hour12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(buf, bufSize, "%u:%02u %s", hour12, minute, hour < 12 ? "AM" : "PM");
  } else {
    snprintf(buf, bufSize, "%02u:%02u", hour, minute);
  }
}
