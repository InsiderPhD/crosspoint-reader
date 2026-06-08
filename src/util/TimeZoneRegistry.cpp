#include "TimeZoneRegistry.h"

#include <array>

namespace {
constexpr std::array<TimeZonePreset, 29> TIME_ZONE_PRESETS = {{
    {"UTC", "UTC0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Moscow", "MSK-3"},
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"America/Mexico_City", "CST6"},
    {"America/Sao_Paulo", "BRT3"},
    {"America/Buenos_Aires", "ART3"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Nairobi", "EAT-3"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "BDT-6"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
}};
}  // namespace

size_t TimeZoneRegistry::getPresetCount() { return TIME_ZONE_PRESETS.size(); }

uint8_t TimeZoneRegistry::clampPresetIndex(const uint8_t index) {
  return index < TIME_ZONE_PRESETS.size() ? index : DEFAULT_TIME_ZONE_INDEX;
}

const TimeZonePreset& TimeZoneRegistry::getPreset(const uint8_t index) {
  return TIME_ZONE_PRESETS[clampPresetIndex(index)];
}

const char* TimeZoneRegistry::getPresetLabel(const uint8_t index) { return getPreset(index).label; }

const char* TimeZoneRegistry::getPresetPosixTz(const uint8_t index) { return getPreset(index).posixTz; }
