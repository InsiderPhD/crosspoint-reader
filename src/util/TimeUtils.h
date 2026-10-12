#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace TimeUtils {

// Push the active zone + DST mode (settings) into the C library. Cheap when
// nothing changed, so callers invoke it before every local-time conversion.
void configureTimezone();
// Persist a zone picked from TimeZoneRegistry's table and apply it. No write
// when it is already the stored zone.
void setTimeZone(uint8_t index);
// Table index of the zone in effect, or TimeZoneRegistry::NOT_CHOSEN when a
// migrated quarter-hour offset has no table entry.
uint8_t getTimeZoneIndex();
bool syncTimeWithNtp(uint32_t timeoutMs = 5000);
bool isClockValid();
bool isClockValid(uint32_t epochSeconds);
uint32_t getAuthoritativeTimestamp();
uint32_t getCurrentValidTimestamp();
bool setCurrentDate(int year, unsigned month, unsigned day, uint32_t* epochSeconds = nullptr);
uint32_t getLocalDayOrdinal(uint32_t epochSeconds);
// Local wall-clock hour (0-23) for an epoch timestamp. False when the clock is
// not trustworthy, in which case hourOut is untouched.
bool getLocalHour(uint32_t epochSeconds, uint8_t& hourOut);
// Local wall-clock minute of the day (0-1439) for the system clock right now.
// False when the clock is not trustworthy, in which case minuteOut is untouched.
bool getLocalMinuteOfDay(uint16_t& minuteOut);
uint32_t getDayOrdinalForDate(int year, unsigned month, unsigned day);
bool getDateFromDayOrdinal(uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day);
bool wasTimeSyncedThisBoot();
// Seed the system clock from the board's RTC at boot. This is the only source
// of a date for a device that never sees WiFi. False when the board has no RTC,
// the chip was never set, or it reports a pre-2024 (so untrustworthy) time.
bool seedClockFromRtc();
// Name of the zone in effect. Usually a flash string; `buf` (at least
// TimeZoneRegistry::OFFSET_BUFFER_SIZE) is only written for a bare offset.
const char* getCurrentTimeZoneLabel(char* buf, size_t bufSize);
// The RTC-backed status-bar clock in the active zone and the user's 12/24h
// format (needs >= 9 bytes). False on boards without an RTC.
bool formatClock(char* buf, size_t bufSize);
std::string formatDate(uint32_t epochSeconds, bool appendBang = false);
std::string formatDateTime(uint32_t epochSeconds, bool appendBang = false);
std::string formatTime(uint32_t epochSeconds);
std::string formatShortDate(uint32_t epochSeconds);
std::string formatDateParts(int year, unsigned month, unsigned day, bool appendBang = false);
std::string formatMonthYear(int year, unsigned month);

}  // namespace TimeUtils
