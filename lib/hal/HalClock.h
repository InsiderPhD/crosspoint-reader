#pragma once

#include <Arduino.h>
#include <Rtc.h>
#include <time.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

// Thin HAL wrapper over the freeink-sdk RTC driver (freeink::Rtc). The driver
// targets whatever RTC the active board profile describes — on the X3 that is the
// DS3231 at 0x68. All chip/register handling lives in the SDK; this class only adds
// the app-facing conveniences (device gating, a cached reading for the status bar,
// local-time conversion, and NTP sync). The X3 profile must be active (main.cpp selects it right after
// hardware detection) before begin() is called, or the SDK reports no RTC.
class HalClock {
  bool _available = false;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are right all year.
  // Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
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

  // Set the process-wide POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") that
  // every local-time read here, and every localtime_r() elsewhere, applies.
  // nullptr/empty falls back to UTC. Works on boards without an RTC too.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Returns false if RTC is not available or has never produced a valid reading.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // use12Hour is deliberately not defaulted: always pass it.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour) const;

  // Read the RTC as a UTC epoch. False when there is no RTC, on an I2C error,
  // or when the chip reports its oscillator stopped (never set / flat backup
  // cell), so a garbage time can never reach the system clock.
  bool getUtcEpoch(uint32_t& outEpoch) const;

  // Store a UTC epoch in the RTC, so the date survives a power cycle. Called
  // whenever the system clock becomes trustworthy (NTP, or a manual date).
  bool setUtcEpoch(uint32_t epoch);

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Set the ESP32 system clock (UTC) from NTP; needs no RTC. Requires WiFi to be
  // connected and blocks the caller for up to timeoutMs. Runs one sync at a
  // time: a call made while another task is syncing waits for it first. The
  // SNTP client is started and stopped inside the call, so nothing keeps
  // polling afterwards.
  bool syncSystemClockFromNtp(uint32_t timeoutMs = 5000);

 private:
  // Write the full date and time to the RTC (freeink::Rtc::set writes the whole
  // struct; only the time is read back for the status-bar clock). UTC in, 2000-2099.
  bool writeDateTime(int year, unsigned month, unsigned day, uint8_t hour, uint8_t minute, uint8_t second);
};
