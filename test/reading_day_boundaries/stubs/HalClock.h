#pragma once
// Host stand-in for lib/hal/HalClock. setTimezone() does exactly what the real
// one does (setenv + tzset), which is the only behaviour the day-boundary code
// depends on; the RTC and NTP sides report "not there".
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>

class HalClock {
 public:
  void setTimezone(const char* posixTz) {
    setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
    tzset();
  }
  bool syncSystemClockFromNtp(uint32_t = 5000) { return false; }
  bool setUtcEpoch(uint32_t) { return false; }
  bool getUtcEpoch(uint32_t&) const { return false; }
  bool formatTime(char*, size_t, bool) const { return false; }
};
inline HalClock halClock;
