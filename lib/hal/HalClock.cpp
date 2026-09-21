#include "HalClock.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  // Any board whose profile declares an RTC: the X3's DS3231, the X4 Pro's
  // PCF8563 on the shared touch bus. Boards without one (the C3 X4) have no
  // rtcAddr, and Rtc::begin() refuses those anyway — this just avoids probing.
  //
  // freeink::Rtc reads the RTC's address, bus and chip type from the active board
  // profile (BoardConfig::ACTIVE). main.cpp selects the X3 profile right after
  // hardware detection; if that hasn't happened begin() below simply reports no RTC.
  if (!BoardConfig::hasRtc() || !_rtc.begin()) {
    LOG_INF("CLK", "RTC not found");
    _available = false;
    return;
  }

  _available = true;
  LOG_INF("CLK", "RTC found");

  // Prime the HH:MM cache with an initial read.
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  freeink::Rtc::DateTime dt;
  if (!_rtc.now(dt)) {
    // I2C error or oscillator stopped — reuse the last good value if we have one.
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

namespace {
// days::from_civil (Howard Hinnant): calendar date -> days since 1970-01-01.
int32_t daysFromCivil(int year, const unsigned month, const unsigned day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const auto yoe = static_cast<uint32_t>(year - era * 400);
  const uint32_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}
}  // namespace

bool HalClock::getUtcEpoch(uint32_t& outEpoch) const {
  if (!_available) return false;

  freeink::Rtc::DateTime dt;
  if (!_rtc.now(dt)) return false;  // I2C error, or oscillator stopped: unusable
  if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 || dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
    LOG_ERR("CLK", "RTC returned an implausible date: %04u-%02u-%02u %02u:%02u", dt.year, dt.month, dt.day, dt.hour,
            dt.minute);
    return false;
  }

  const int32_t days = daysFromCivil(dt.year, dt.month, dt.day);
  if (days < 0) return false;
  outEpoch = static_cast<uint32_t>(days) * 86400u + dt.hour * 3600u + dt.minute * 60u + dt.second;
  return true;
}

bool HalClock::setUtcEpoch(const uint32_t epoch) {
  if (!_available) return false;
  const time_t when = static_cast<time_t>(epoch);
  struct tm utc;
  if (gmtime_r(&when, &utc) == nullptr) return false;
  return writeDateTime(utc.tm_year + 1900, static_cast<unsigned>(utc.tm_mon + 1), static_cast<unsigned>(utc.tm_mday),
                       static_cast<uint8_t>(utc.tm_hour), static_cast<uint8_t>(utc.tm_min),
                       static_cast<uint8_t>(utc.tm_sec));
}

bool HalClock::writeDateTime(int year, unsigned month, unsigned day, uint8_t hour, uint8_t minute, uint8_t second) {
  if (!_available) return false;
  if (year < 2000 || year > 2099) {
    LOG_ERR("CLK", "writeDateTime year out of range: %d", year);
    return false;
  }
  assert(month >= 1 && month <= 12);
  assert(day >= 1 && day <= 31);
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);

  freeink::Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(year);
  dt.month = static_cast<uint8_t>(month);
  dt.day = static_cast<uint8_t>(day);
  dt.hour = hour;
  dt.minute = minute;
  dt.second = second;
  dt.weekday = 0;  // day-of-week is written by the SDK but never read back here

  if (!_rtc.set(dt)) {
    LOG_ERR("CLK", "Failed to write date/time to DS3231");
    return false;
  }

  // Invalidate the HH:MM cache so the next status-bar read fetches fresh data.
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (writeDateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                        timeinfo.tm_min, timeinfo.tm_sec)) {
        LOG_INF("CLK", "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
