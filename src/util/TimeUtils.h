#pragma once

#include <cstdint>
#include <string>

namespace TimeUtils {

void configureTimezone();
void stopNtp();
bool syncTimeWithNtp(uint32_t timeoutMs = 5000);
bool isClockValid();
bool isClockValid(uint32_t epochSeconds);
uint32_t getAuthoritativeTimestamp();
uint32_t getCurrentValidTimestamp();
// Best date the device has, without going back to the network: the live system
// clock when it is valid, otherwise the timestamp the last successful NTP or
// manual date-set persisted (APP_STATE.lastKnownValidTimestamp). Returns 0 only
// when this device has never known the date at all. Use this for "which day is
// it" decisions that must not drag the user through another sync; use
// getAuthoritativeTimestamp() when only a this-boot-verified clock will do.
uint32_t getBestKnownTimestamp();
bool setCurrentDate(int year, unsigned month, unsigned day, uint32_t* epochSeconds = nullptr);
uint32_t getLocalDayOrdinal(uint32_t epochSeconds);
uint32_t getDayOrdinalForDate(int year, unsigned month, unsigned day);
bool getDateFromDayOrdinal(uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day);
bool wasTimeSyncedThisBoot();
const char* getCurrentTimeZoneLabel();
std::string formatDate(uint32_t epochSeconds, bool appendBang = false);
std::string formatDateTime(uint32_t epochSeconds, bool appendBang = false);
std::string formatTime(uint32_t epochSeconds);
std::string formatShortDate(uint32_t epochSeconds);
std::string formatDateParts(int year, unsigned month, unsigned day, bool appendBang = false);
std::string formatMonthYear(int year, unsigned month);

}  // namespace TimeUtils
