#pragma once

#include <HalFrontlight.h>

#include "CrossPointSettings.h"

// READER_ACTION_TOGGLE_FRONTLIGHT. Off stores 0 like the brightness picker's
// own Off rung, so every other screen agrees the light is off; on restores the
// last non-zero brightness saveToFile() captured.
namespace FrontlightToggle {

// Used only when no brightness was ever set: a dim reading light, not a
// full-power flash in a dark room.
inline constexpr uint8_t FALLBACK_BRIGHTNESS = 10;

// Returns false (and changes nothing) on a board without a frontlight.
inline bool toggle() {
  if (!halFrontlight.present()) return false;
  if (SETTINGS.frontlightBrightness > 0) {
    SETTINGS.frontlightLastBrightness = SETTINGS.frontlightBrightness;
    SETTINGS.frontlightBrightness = 0;
  } else {
    SETTINGS.frontlightBrightness =
        SETTINGS.frontlightLastBrightness > 0 ? SETTINGS.frontlightLastBrightness : FALLBACK_BRIGHTNESS;
  }
  halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
  SETTINGS.saveToFile();
  return true;
}

}  // namespace FrontlightToggle
