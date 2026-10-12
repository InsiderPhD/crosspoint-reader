#pragma once
// Host stand-in carrying only the clock fields TimeUtils.cpp reads. Defaults
// match CrossPointSettings.h (255 = TimeZoneRegistry::NOT_CHOSEN, 48 = UTC).
#include <cstdint>

struct CrossPointSettings {
  uint8_t clockTimezone = 255;
  uint8_t timeZonePreset = 0;
  uint8_t clockUtcOffsetQ = 48;
  uint8_t clockDst = 0;
  uint8_t clockFormat = 0;
  bool saveToFile() const { return true; }
  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};
#define SETTINGS CrossPointSettings::getInstance()
