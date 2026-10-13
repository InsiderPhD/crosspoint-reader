#include "TimeUtils.h"

#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#ifdef SIMULATOR
#include <esp_sntp.h>  // host shim: reports the (already correct) host clock as synced
#endif
#include <sys/time.h>

#include <algorithm>
#include <climits>
#include <ctime>

#include "CrossPointSettings.h"
#include "TimeZoneRegistry.h"

namespace {
constexpr uint32_t VALID_CLOCK_THRESHOLD = 1704067200UL;  // 2024-01-01 UTC
bool syncedThisBoot = false;
// The clock came from the RTC rather than NTP. Kept separate from
// syncedThisBoot, which callers also read as "the network worked this boot"
// (ProgressAutoSync) — an offline device must not start attempting pushes.
bool seededFromRtcThisBoot = false;
// The (zone, DST mode) pair last pushed into the C library, so the many callers
// of configureTimezone() cost three compares rather than a setenv each.
constexpr uint32_t TIME_ZONE_NOT_CONFIGURED = UINT32_MAX;
uint32_t configuredTimeZoneKey = TIME_ZONE_NOT_CONFIGURED;

TimeZoneRegistry::Resolved activeTimeZone() {
  return TimeZoneRegistry::resolve(SETTINGS.clockTimezone, SETTINGS.timeZonePreset, SETTINGS.clockUtcOffsetQ);
}

bool isLeapYear(const int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

unsigned getDaysInMonth(const int year, const unsigned month) {
  static constexpr unsigned DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    return isLeapYear(year) ? 29U : 28U;
  }
  if (month < 1 || month > 12) {
    return 0;
  }
  return DAYS_PER_MONTH[month - 1];
}

std::string formatDateBuffer(const int year, const unsigned month, const unsigned day, const bool appendBang) {
  const char* bang = appendBang ? "!" : "";
  char buffer[24];

  // Keep date formatting deterministic for now (dd/mm/yyyy) and avoid adding
  // new settings keys for this first integration pass.
  snprintf(buffer, sizeof(buffer), "%02u/%02u/%04d%s", day, month, year, bang);

  return buffer;
}

int32_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

void civilFromDays(int z, int& year, unsigned& month, unsigned& day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned dayOfEra = static_cast<unsigned>(z - era * 146097);
  const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  year = static_cast<int>(yearOfEra) + era * 400;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned monthPart = (5 * dayOfYear + 2) / 153;
  day = dayOfYear - (153 * monthPart + 2) / 5 + 1;
  month = monthPart + (monthPart < 10 ? 3 : -9);
  year += (month <= 2);
}
}  // namespace

void TimeUtils::configureTimezone() {
  const TimeZoneRegistry::Resolved zone = activeTimeZone();
  const uint8_t dstMode =
      SETTINGS.clockDst < TimeZoneRegistry::DST_MODE_COUNT ? SETTINGS.clockDst : uint8_t{TimeZoneRegistry::DST_AUTO};
  // offsetQ is in the key because a bare migrated offset has no table index.
  const uint32_t key = (static_cast<uint32_t>(dstMode) << 24) | (static_cast<uint32_t>(zone.index) << 16) |
                       static_cast<uint16_t>(zone.offsetQ);
  if (configuredTimeZoneKey == key) {
    return;
  }
  char posixTz[TimeZoneRegistry::POSIX_TZ_BUFFER_SIZE];
  TimeZoneRegistry::buildPosixTz(zone, dstMode, posixTz, sizeof(posixTz));
  // One setenv for the whole process: the RTC-backed status-bar clock and every
  // localtime_r() below share the rule.
  halClock.setTimezone(posixTz);
  configuredTimeZoneKey = key;
  LOG_DBG("TIME", "Time zone set to %s", posixTz);
}

void TimeUtils::setTimeZone(const uint8_t index) {
  if (index >= TimeZoneRegistry::getPresetCount() || index == SETTINGS.clockTimezone) {
    return;
  }
  SETTINGS.clockTimezone = index;
  // Mirror the choice into the retired fields so older firmware reading this
  // settings.json lands as close as its own model allows.
  SETTINGS.timeZonePreset = TimeZoneRegistry::legacyPresetFor(index);
  SETTINGS.clockUtcOffsetQ = TimeZoneRegistry::legacyOffsetQFor(index);
  SETTINGS.saveToFile();
  configureTimezone();
}

uint8_t TimeUtils::getTimeZoneIndex() { return activeTimeZone().index; }

bool TimeUtils::syncTimeWithNtp(const uint32_t timeoutMs) {
  configureTimezone();

#ifdef SIMULATOR
  (void)timeoutMs;
  const bool synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
#else
  // The HAL runs the SNTP client through esp-netif, one sync at a time, and
  // stops it again before returning.
  const bool synced = halClock.syncSystemClockFromNtp(timeoutMs);
#endif
  if (!synced) {
    return false;
  }

  const time_t currentTime = time(nullptr);
  if (!isClockValid(static_cast<uint32_t>(currentTime))) {
    return false;
  }

  syncedThisBoot = true;
  // Park it in the RTC so the date survives the next power cycle even if
  // this is the last network the device ever sees.
  halClock.setUtcEpoch(static_cast<uint32_t>(currentTime));
  return true;
}

bool TimeUtils::isClockValid() { return isClockValid(static_cast<uint32_t>(time(nullptr))); }

bool TimeUtils::isClockValid(const uint32_t epochSeconds) { return epochSeconds >= VALID_CLOCK_THRESHOLD; }

uint32_t TimeUtils::getAuthoritativeTimestamp() {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if ((syncedThisBoot || seededFromRtcThisBoot) && isClockValid(now)) {
    return now;
  }
  return 0;
}

uint32_t TimeUtils::getCurrentValidTimestamp() {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  return isClockValid(now) ? now : 0;
}

bool TimeUtils::setCurrentDate(const int year, const unsigned month, const unsigned day, uint32_t* epochSeconds) {
  configureTimezone();

  const unsigned daysInMonth = getDaysInMonth(year, month);
  if (day < 1 || daysInMonth == 0 || day > daysInMonth) {
    return false;
  }

  tm localTime = {};
  localTime.tm_year = year - 1900;
  localTime.tm_mon = static_cast<int>(month) - 1;
  localTime.tm_mday = static_cast<int>(day);
  localTime.tm_hour = 12;
  localTime.tm_min = 0;
  localTime.tm_sec = 0;
  localTime.tm_isdst = -1;

  const time_t epoch = mktime(&localTime);
  if (epoch < 0 || !isClockValid(static_cast<uint32_t>(epoch))) {
    return false;
  }

  timeval tv{};
  tv.tv_sec = epoch;
  if (settimeofday(&tv, nullptr) != 0) {
    return false;
  }

  syncedThisBoot = true;
  // A hand-entered date is the offline user's only input; keep it across boots.
  halClock.setUtcEpoch(static_cast<uint32_t>(epoch));
  if (epochSeconds) {
    *epochSeconds = static_cast<uint32_t>(epoch);
  }
  return true;
}

uint32_t TimeUtils::getLocalDayOrdinal(const uint32_t epochSeconds) {
  configureTimezone();

  if (!isClockValid(epochSeconds)) {
    return 0;
  }

  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return epochSeconds / 86400UL;
  }

  return static_cast<uint32_t>(daysFromCivil(localTime.tm_year + 1900, static_cast<unsigned>(localTime.tm_mon + 1),
                                             static_cast<unsigned>(localTime.tm_mday)));
}

bool TimeUtils::getLocalMinuteOfDay(uint16_t& minuteOut) {
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (!isClockValid(now)) {
    return false;
  }
  configureTimezone();
  time_t currentTime = static_cast<time_t>(now);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return false;
  }
  minuteOut = static_cast<uint16_t>(localTime.tm_hour * 60 + localTime.tm_min);
  return true;
}

bool TimeUtils::getLocalHour(const uint32_t epochSeconds, uint8_t& hourOut) {
  configureTimezone();

  if (!isClockValid(epochSeconds)) {
    return false;
  }

  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return false;
  }

  hourOut = static_cast<uint8_t>(localTime.tm_hour);
  return true;
}

uint32_t TimeUtils::getDayOrdinalForDate(const int year, const unsigned month, const unsigned day) {
  return static_cast<uint32_t>(daysFromCivil(year, month, day));
}

bool TimeUtils::getDateFromDayOrdinal(const uint32_t dayOrdinal, int& year, unsigned& month, unsigned& day) {
  civilFromDays(static_cast<int>(dayOrdinal), year, month, day);
  return true;
}

bool TimeUtils::wasTimeSyncedThisBoot() { return syncedThisBoot; }

bool TimeUtils::seedClockFromRtc() {
  uint32_t epoch = 0;
  if (!halClock.getUtcEpoch(epoch) || !isClockValid(epoch)) {
    return false;
  }

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(epoch);
  if (settimeofday(&tv, nullptr) != 0) {
    return false;
  }
  seededFromRtcThisBoot = true;
  configureTimezone();
  LOG_INF("TIME", "System clock seeded from RTC (%u)", static_cast<unsigned>(epoch));
  return true;
}

const char* TimeUtils::getCurrentTimeZoneLabel(char* buf, const size_t bufSize) {
  return TimeZoneRegistry::label(activeTimeZone(), buf, bufSize);
}

bool TimeUtils::formatClock(char* buf, const size_t bufSize) {
  configureTimezone();
  return halClock.formatTime(buf, bufSize, SETTINGS.clockFormat == 1);
}

std::string TimeUtils::formatDate(const uint32_t epochSeconds, const bool appendBang) {
  if (!isClockValid(epochSeconds)) {
    return "";
  }

  configureTimezone();
  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return "";
  }

  return formatDateBuffer(localTime.tm_year + 1900, static_cast<unsigned>(localTime.tm_mon + 1),
                          static_cast<unsigned>(localTime.tm_mday), appendBang);
}

std::string TimeUtils::formatDateTime(const uint32_t epochSeconds, const bool appendBang) {
  if (!isClockValid(epochSeconds)) {
    return "";
  }

  configureTimezone();
  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return "";
  }

  return formatDateBuffer(localTime.tm_year + 1900, static_cast<unsigned>(localTime.tm_mon + 1),
                          static_cast<unsigned>(localTime.tm_mday), appendBang) +
         " " + (localTime.tm_hour < 10 ? "0" : "") + std::to_string(localTime.tm_hour) + ":" +
         (localTime.tm_min < 10 ? "0" : "") + std::to_string(localTime.tm_min);
}

std::string TimeUtils::formatTime(const uint32_t epochSeconds) {
  if (!isClockValid(epochSeconds)) {
    return "";
  }

  configureTimezone();
  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return "";
  }

  // Same two shapes as HalClock::formatTime, so the Clock Format setting reads
  // the same whether the time comes from the RTC or the system clock.
  char buffer[12];
  if (SETTINGS.clockFormat == 1) {
    int hour12 = localTime.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour12, localTime.tm_min, localTime.tm_hour >= 12 ? "PM" : "AM");
  } else {
    snprintf(buffer, sizeof(buffer), "%02d:%02d", localTime.tm_hour, localTime.tm_min);
  }
  return buffer;
}

// Unpadded day/month (e.g. "10/7") — matches the dd/mm order used elsewhere.
std::string TimeUtils::formatShortDate(const uint32_t epochSeconds) {
  if (!isClockValid(epochSeconds)) {
    return "";
  }

  configureTimezone();
  time_t currentTime = static_cast<time_t>(epochSeconds);
  tm localTime = {};
  if (localtime_r(&currentTime, &localTime) == nullptr) {
    return "";
  }

  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%d/%d", localTime.tm_mday, localTime.tm_mon + 1);
  return buffer;
}

std::string TimeUtils::formatDateParts(const int year, const unsigned month, const unsigned day,
                                       const bool appendBang) {
  return formatDateBuffer(year, month, day, appendBang);
}

std::string TimeUtils::formatMonthYear(const int year, const unsigned month) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02u/%04d", month, year);
  return buffer;
}
