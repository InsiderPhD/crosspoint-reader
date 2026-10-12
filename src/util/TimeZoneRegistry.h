#pragma once

#include <cstddef>
#include <cstdint>

// Curated time-zone list for every clock on the device (status-bar clock, dates,
// reading-stats day boundaries, frontlight schedules). Each entry carries the
// POSIX TZ rule newlib applies in localtime_r(), so zones with daylight saving
// show the right wall time all year from a UTC-keeping clock.
//
// The table is constexpr and lives in flash; nothing here allocates.
//
// APPEND ONLY: CrossPointSettings::clockTimezone persists an index into this
// table, so reordering or removing entries retargets users' saved zones.
struct TimeZonePreset {
  const char* label;    // shown in the picker; city names stay untranslated
  const char* posixTz;  // POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
  int16_t stdOffsetQ;   // standard (non-DST) offset from UTC in quarter hours
};

namespace TimeZoneRegistry {

// CrossPointSettings::clockTimezone value meaning "never chosen": the zone is
// then derived from the two retired settings (see resolve()).
constexpr uint8_t NOT_CHOSEN = 255;

// DST handling, persisted in CrossPointSettings::clockDst. Append only.
enum DstMode : uint8_t { DST_AUTO = 0, DST_ON = 1, DST_OFF = 2, DST_MODE_COUNT };

// The zone in effect. `index` is a table index, or NOT_CHOSEN when a retired
// quarter-hour offset has no table entry and applies as a bare fixed offset.
struct Resolved {
  uint8_t index;
  int16_t offsetQ;  // standard offset in quarter hours (the fixed offset when index == NOT_CHOSEN)
};

// Buffer sizes that fit every value the functions below produce.
constexpr size_t POSIX_TZ_BUFFER_SIZE = 40;
constexpr size_t OFFSET_BUFFER_SIZE = 12;

size_t getPresetCount();
const TimeZonePreset& getPreset(uint8_t index);  // out-of-range falls back to UTC

// Pick the zone from the persisted fields. `clockTimezone` wins when valid.
// Otherwise this migrates the retired settings without moving the clock:
//  - `legacyOffsetQ` (biased quarter hours, 48 = UTC) drove the status-bar clock
//    and was a FIXED offset, so it maps to a fixed-offset zone, never a region;
//  - `legacyPreset` indexed the old 29-zone list that drove dates and stats.
// When both are set, the old preset is kept only if the offset agrees with it
// (its standard offset, or that plus an hour for a zone that observes DST);
// otherwise the offset wins, because it is the clock the user was looking at.
Resolved resolve(uint8_t clockTimezone, uint8_t legacyPreset, uint8_t legacyOffsetQ);

// POSIX TZ rule for a resolved zone under a DST mode. DST_AUTO uses the zone's
// own rule; DST_ON / DST_OFF pin a fixed offset (standard, plus an hour for ON).
void buildPosixTz(const Resolved& zone, uint8_t dstMode, char* buf, size_t bufSize);

// "UTC", "UTC+5:30", "UTC-3:00".
void formatOffset(int offsetQ, char* buf, size_t bufSize);

// Display name of a resolved zone: the table label, or the formatted offset for
// a bare fixed offset. `buf` is only written in the latter case.
const char* label(const Resolved& zone, char* buf, size_t bufSize);

// Values for the two retired fields that best describe table entry `index`, so
// a settings.json written by this firmware still reads sensibly on an older one.
uint8_t legacyPresetFor(uint8_t index);   // 0 (UTC) when the old list has no match
uint8_t legacyOffsetQFor(uint8_t index);  // biased quarter hours

}  // namespace TimeZoneRegistry
