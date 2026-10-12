#include "TimeZoneRegistry.h"

#include <cstdio>
#include <cstring>

namespace {

// Ordered west to east by standard offset, then the fixed offsets. APPEND ONLY
// (see header). Same list and order as upstream CrossPoint's util/Timezones.cpp.
constexpr TimeZonePreset TABLE[] = {
    {"Midway", "SST11", -44},
    {"Honolulu", "HST10", -40},
    {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0", -36},
    {"Los Angeles / Vancouver", "PST8PDT,M3.2.0,M11.1.0", -32},
    {"Denver / Edmonton", "MST7MDT,M3.2.0,M11.1.0", -28},
    {"Phoenix", "MST7", -28},
    {"Chicago / Winnipeg", "CST6CDT,M3.2.0,M11.1.0", -24},
    {"Mexico City", "CST6", -24},
    {"New York / Toronto", "EST5EDT,M3.2.0,M11.1.0", -20},
    {"Bogota / Lima", "COT5", -20},
    {"Halifax", "AST4ADT,M3.2.0,M11.1.0", -16},
    {"Caracas / La Paz", "VET4", -16},
    {"Santiago", "CLT4CLST,M9.1.6/24,M4.1.6/24", -16},
    {"St. John's", "NST3:30NDT,M3.2.0,M11.1.0", -14},
    {"Buenos Aires / Montevideo", "ART3", -12},
    {"Sao Paulo", "BRT3", -12},
    {"Azores", "AZOT1AZOST,M3.5.0/0,M10.5.0/1", -4},
    {"UTC", "UTC0", 0},
    {"London / Dublin / Lisbon", "GMT0BST,M3.5.0/1,M10.5.0", 0},
    {"Berlin / Paris / Madrid / Rome", "CET-1CEST,M3.5.0,M10.5.0/3", 4},
    {"Lagos / Algiers", "WAT-1", 4},
    {"Athens / Helsinki / Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4", 8},
    {"Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", 8},
    {"Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0", 8},
    {"Johannesburg", "SAST-2", 8},
    {"Moscow / Istanbul / Riyadh", "MSK-3", 12},
    {"Nairobi", "EAT-3", 12},
    {"Tehran", "IRST-3:30", 14},
    {"Dubai / Tbilisi", "GST-4", 16},
    {"Kabul", "AFT-4:30", 18},
    {"Karachi / Tashkent", "PKT-5", 20},
    {"India / Colombo", "IST-5:30", 22},
    {"Kathmandu", "NPT-5:45", 23},
    {"Dhaka / Almaty", "BST-6", 24},
    {"Yangon", "MMT-6:30", 26},
    {"Bangkok / Jakarta / Hanoi", "ICT-7", 28},
    {"China / Hong Kong / Taipei", "CST-8", 32},
    {"Singapore / Manila / Kuala Lumpur", "SGT-8", 32},
    {"Perth", "AWST-8", 32},
    {"Tokyo / Seoul", "JST-9", 36},
    {"Darwin", "ACST-9:30", 38},
    {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", 38},
    {"Brisbane / Guam", "AEST-10", 40},
    {"Sydney / Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3", 40},
    {"Honiara / Noumea", "SBT-11", 44},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", 48},
    {"Fiji", "FJT-12", 48},
    {"Nuku'alofa", "TOT-13", 52},
    {"Kiritimati", "LINT-14", 56},
    // Fixed offsets, no DST — the manual fallback for zones the named list
    // misses. Note the POSIX offset sign is inverted relative to the label.
    {"UTC-12:00", "UTC12", -48},
    {"UTC-11:30", "UTC11:30", -46},
    {"UTC-11:00", "UTC11", -44},
    {"UTC-10:30", "UTC10:30", -42},
    {"UTC-10:00", "UTC10", -40},
    {"UTC-09:30", "UTC9:30", -38},
    {"UTC-09:00", "UTC9", -36},
    {"UTC-08:30", "UTC8:30", -34},
    {"UTC-08:00", "UTC8", -32},
    {"UTC-07:30", "UTC7:30", -30},
    {"UTC-07:00", "UTC7", -28},
    {"UTC-06:30", "UTC6:30", -26},
    {"UTC-06:00", "UTC6", -24},
    {"UTC-05:30", "UTC5:30", -22},
    {"UTC-05:00", "UTC5", -20},
    {"UTC-04:30", "UTC4:30", -18},
    {"UTC-04:00", "UTC4", -16},
    {"UTC-03:30", "UTC3:30", -14},
    {"UTC-03:00", "UTC3", -12},
    {"UTC-02:30", "UTC2:30", -10},
    {"UTC-02:00", "UTC2", -8},
    {"UTC-01:30", "UTC1:30", -6},
    {"UTC-01:00", "UTC1", -4},
    {"UTC-00:30", "UTC0:30", -2},
    {"UTC+00:30", "UTC-0:30", 2},
    {"UTC+01:00", "UTC-1", 4},
    {"UTC+01:30", "UTC-1:30", 6},
    {"UTC+02:00", "UTC-2", 8},
    {"UTC+02:30", "UTC-2:30", 10},
    {"UTC+03:00", "UTC-3", 12},
    {"UTC+03:30", "UTC-3:30", 14},
    {"UTC+04:00", "UTC-4", 16},
    {"UTC+04:30", "UTC-4:30", 18},
    {"UTC+05:00", "UTC-5", 20},
    {"UTC+05:30", "UTC-5:30", 22},
    {"UTC+05:45", "UTC-5:45", 23},
    {"UTC+06:00", "UTC-6", 24},
    {"UTC+06:30", "UTC-6:30", 26},
    {"UTC+07:00", "UTC-7", 28},
    {"UTC+07:30", "UTC-7:30", 30},
    {"UTC+08:00", "UTC-8", 32},
    {"UTC+08:30", "UTC-8:30", 34},
    {"UTC+08:45", "UTC-8:45", 35},
    {"UTC+09:00", "UTC-9", 36},
    {"UTC+09:30", "UTC-9:30", 38},
    {"UTC+10:00", "UTC-10", 40},
    {"UTC+10:30", "UTC-10:30", 42},
    {"UTC+11:00", "UTC-11", 44},
    {"UTC+11:30", "UTC-11:30", 46},
    {"UTC+12:00", "UTC-12", 48},
    {"UTC+12:30", "UTC-12:30", 50},
    {"UTC+12:45", "UTC-12:45", 51},
    {"UTC+13:00", "UTC-13", 52},
    {"UTC+13:30", "UTC-13:30", 54},
    {"UTC+14:00", "UTC-14", 56},
};
constexpr size_t TABLE_COUNT = sizeof(TABLE) / sizeof(TABLE[0]);
constexpr uint8_t UTC_INDEX = 17;
constexpr uint8_t FIXED_START = 49;  // first "UTC+-HH:MM" entry
static_assert(TABLE[UTC_INDEX].stdOffsetQ == 0 && TABLE[UTC_INDEX].posixTz[3] == '0', "UTC_INDEX must be UTC");
static_assert(TABLE[FIXED_START].stdOffsetQ == -48, "FIXED_START must be the UTC-12:00 entry");
static_assert(TABLE_COUNT < TimeZoneRegistry::NOT_CHOSEN, "255 is the 'never chosen' sentinel");

// The retired 29-zone list (CrossPointSettings::timeZonePreset) -> TABLE index.
// Position in this array IS the old persisted index; never reorder.
constexpr uint8_t LEGACY_PRESET_TO_INDEX[] = {
    17,  // UTC
    18,  // Europe/London
    19,  // Europe/Madrid
    19,  // Europe/Paris
    19,  // Europe/Berlin
    19,  // Europe/Rome
    21,  // Europe/Athens
    21,  // Europe/Helsinki
    25,  // Europe/Moscow
    8,   // America/New_York
    6,   // America/Chicago
    4,   // America/Denver
    3,   // America/Los_Angeles
    7,   // America/Mexico_City
    15,  // America/Sao_Paulo
    14,  // America/Buenos_Aires
    24,  // Africa/Johannesburg
    26,  // Africa/Nairobi
    28,  // Asia/Dubai
    30,  // Asia/Karachi
    31,  // Asia/Kolkata
    33,  // Asia/Dhaka
    35,  // Asia/Bangkok
    37,  // Asia/Singapore
    36,  // Asia/Shanghai
    39,  // Asia/Tokyo
    39,  // Asia/Seoul
    43,  // Australia/Sydney
    45,  // Pacific/Auckland
};
constexpr size_t LEGACY_PRESET_COUNT = sizeof(LEGACY_PRESET_TO_INDEX);
static_assert(LEGACY_PRESET_COUNT == 29, "the retired list had exactly 29 zones");

constexpr uint8_t LEGACY_OFFSET_UTC = 48;  // bias of the retired quarter-hour offset
constexpr uint8_t LEGACY_OFFSET_MAX = 104;

bool observesDst(const TimeZonePreset& zone) { return strchr(zone.posixTz, ',') != nullptr; }

}  // namespace

size_t TimeZoneRegistry::getPresetCount() { return TABLE_COUNT; }

const TimeZonePreset& TimeZoneRegistry::getPreset(const uint8_t index) {
  return TABLE[index < TABLE_COUNT ? index : UTC_INDEX];
}

TimeZoneRegistry::Resolved TimeZoneRegistry::resolve(const uint8_t clockTimezone, const uint8_t legacyPreset,
                                                     const uint8_t legacyOffsetQ) {
  if (clockTimezone < TABLE_COUNT) return {clockTimezone, TABLE[clockTimezone].stdOffsetQ};

  const bool hasPreset = legacyPreset != 0 && legacyPreset < LEGACY_PRESET_COUNT;
  const bool hasOffset = legacyOffsetQ != LEGACY_OFFSET_UTC && legacyOffsetQ <= LEGACY_OFFSET_MAX;
  const int offsetQ = static_cast<int>(legacyOffsetQ) - LEGACY_OFFSET_UTC;

  if (hasPreset) {
    const uint8_t index = LEGACY_PRESET_TO_INDEX[legacyPreset];
    const TimeZonePreset& zone = TABLE[index];
    const bool agrees = offsetQ == zone.stdOffsetQ || (observesDst(zone) && offsetQ == zone.stdOffsetQ + 4);
    if (!hasOffset || agrees) return {index, zone.stdOffsetQ};
  }
  if (hasOffset) {
    for (size_t i = FIXED_START; i < TABLE_COUNT; i++) {
      if (TABLE[i].stdOffsetQ == offsetQ) return {static_cast<uint8_t>(i), TABLE[i].stdOffsetQ};
    }
    // A quarter-hour step the fixed list skips (e.g. UTC+0:15): keep it exact.
    return {NOT_CHOSEN, static_cast<int16_t>(offsetQ)};
  }
  return {UTC_INDEX, 0};
}

void TimeZoneRegistry::buildPosixTz(const Resolved& zone, const uint8_t dstMode, char* buf, const size_t bufSize) {
  if (dstMode != DST_ON && dstMode != DST_OFF && zone.index < TABLE_COUNT) {
    snprintf(buf, bufSize, "%s", TABLE[zone.index].posixTz);
    return;
  }
  // A fixed offset. POSIX writes it with the sign inverted: UTC+2 is "UTC-2".
  const int q = zone.offsetQ + (dstMode == DST_ON ? 4 : 0);
  const int absQ = q < 0 ? -q : q;
  snprintf(buf, bufSize, "UTC%c%d:%02d", q < 0 ? '+' : '-', absQ / 4, (absQ % 4) * 15);
}

void TimeZoneRegistry::formatOffset(const int offsetQ, char* buf, const size_t bufSize) {
  if (offsetQ == 0) {
    snprintf(buf, bufSize, "UTC");
    return;
  }
  const int absQ = offsetQ < 0 ? -offsetQ : offsetQ;
  snprintf(buf, bufSize, "UTC%c%d:%02d", offsetQ < 0 ? '-' : '+', absQ / 4, (absQ % 4) * 15);
}

const char* TimeZoneRegistry::label(const Resolved& zone, char* buf, const size_t bufSize) {
  if (zone.index < TABLE_COUNT) return TABLE[zone.index].label;
  formatOffset(zone.offsetQ, buf, bufSize);
  return buf;
}

uint8_t TimeZoneRegistry::legacyPresetFor(const uint8_t index) {
  for (size_t i = 0; i < LEGACY_PRESET_COUNT; i++) {
    if (LEGACY_PRESET_TO_INDEX[i] == index) return static_cast<uint8_t>(i);
  }
  return 0;
}

uint8_t TimeZoneRegistry::legacyOffsetQFor(const uint8_t index) {
  return static_cast<uint8_t>(getPreset(index).stdOffsetQ + LEGACY_OFFSET_UTC);
}
