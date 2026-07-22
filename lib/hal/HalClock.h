#pragma once

#include <Arduino.h>
#include <Rtc.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

// Thin HAL wrapper over the freeink-sdk RTC driver (freeink::Rtc). The driver
// targets whatever RTC the active board profile describes — on the X3 that is the
// DS3231 at 0x68. All chip/register handling lives in the SDK; this class only adds
// the app-facing conveniences (device gating, an HH:MM cache for the status bar,
// and NTP sync). The X3 profile must be active (main.cpp selects it right after
// hardware detection) before begin() is called, or the SDK reports no RTC.
class HalClock {
  bool _available = false;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;
  // mutable so the const read path (getTime) can call the SDK's non-const now();
  // it mutates only internal bus-ready state, not observable time.
  mutable freeink::Rtc _rtc;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // True if the RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

 private:
  // Write the full date and time to the RTC (freeink::Rtc::set writes the whole
  // struct; only the time is read back for the status-bar clock). UTC in, 2000-2099.
  bool writeDateTime(int year, unsigned month, unsigned day, uint8_t hour, uint8_t minute, uint8_t second);
};
