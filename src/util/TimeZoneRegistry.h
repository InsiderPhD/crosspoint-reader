#pragma once

#include <cstddef>
#include <cstdint>

struct TimeZonePreset {
  const char* label;
  const char* posixTz;
};

namespace TimeZoneRegistry {

constexpr uint8_t DEFAULT_TIME_ZONE_INDEX = 0;

size_t getPresetCount();
uint8_t clampPresetIndex(uint8_t index);
const TimeZonePreset& getPreset(uint8_t index);
const char* getPresetLabel(uint8_t index);
const char* getPresetPosixTz(uint8_t index);

}  // namespace TimeZoneRegistry
