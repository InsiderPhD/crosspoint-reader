#include "HalClock.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

namespace {
// esp_netif_sntp keeps one process-wide state block, and its deinit frees the
// semaphore a concurrent esp_netif_sntp_sync_wait() would be blocked on. Syncs
// are started from the loop task (WiFi flows, Sync clock now) and from the
// silent boot task, so only one may own that state at a time. A flag under a
// spinlock rather than a mutex: nothing to allocate, and it is held for two
// instructions.
portMUX_TYPE ntpSyncMux = portMUX_INITIALIZER_UNLOCKED;
bool ntpSyncBusy = false;

bool tryClaimNtpSync() {
  bool claimed = false;
  portENTER_CRITICAL(&ntpSyncMux);
  if (!ntpSyncBusy) {
    ntpSyncBusy = true;
    claimed = true;
  }
  portEXIT_CRITICAL(&ntpSyncMux);
  return claimed;
}

void releaseNtpSync() {
  portENTER_CRITICAL(&ntpSyncMux);
  ntpSyncBusy = false;
  portEXIT_CRITICAL(&ntpSyncMux);
}

// Runs on the lwIP thread via esp_netif_tcpip_exec(), i.e. under the core lock.
esp_err_t clearDnsCacheLocked(void*) {
  dns_clear_cache();
  return ESP_OK;
}
}  // namespace

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

  // Prime the cache with an initial read.
  struct tm local;
  localTime(local);
}

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    uint32_t epoch = 0;
    if (getUtcEpoch(epoch)) {
      _cachedUtc = static_cast<time_t>(epoch);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      // I2C error or oscillator stopped, and no earlier good value to reuse.
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the "read again" sentinel
  }
  // The cache holds UTC, so a timezone change shows on the very next call
  // without touching the bus. The up-to-10s staleness is the same as before.
  return localtime_r(&_cachedUtc, &out) != nullptr;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
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

  // Invalidate the cache so the next status-bar read fetches the new time.
  _lastPollMs = 0;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (!syncSystemClockFromNtp()) return false;

  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  if (!writeDateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
                     timeinfo.tm_sec)) {
    return false;
  }
  LOG_INF("CLK", "RTC set to %04d-%02d-%02d %02d:%02d:%02d UTC", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
          timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return true;
}

// ---- direct SNTP query ----------------------------------------------------
//
// esp_sntp sends nothing when it starts: IDF ships CONFIG_LWIP_SNTP_STARTUP_DELAY=y with a
// 5000 ms maximum, so the FIRST packet leaves after a uniformly random 0-5 s (a fleet must
// not stampede a public pool at power-on). On a sync the user is waiting for that is 2.5 s of
// dead air on average, and it is compiled into the prebuilt SDK libs where sdkconfig cannot
// reach it. A client SNTP request is one 48-byte datagram, so asking directly costs a few
// lines and skips both the delay and lwIP's serial per-server timeout: the gateway (most home
// routers answer NTP) and one anycast address that needs no DNS are asked at once and the
// first valid answer wins. Fast path only: syncSystemClockFromNtp() falls back to the full
// client whenever it returns false. Ported in shape from witchhunt-reader's quickSntpQuery.
namespace {
constexpr uint16_t NTP_PORT = 123;
constexpr size_t NTP_PACKET_SIZE = 48;
// Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01).
constexpr uint32_t NTP_TO_UNIX_OFFSET = 2208988800UL;
// A malformed or spoofed reply that sets the clock is worse than no sync at all (it would also
// break TLS certificate validity checks), so anything outside a plausible window is dropped.
constexpr time_t NTP_MIN_PLAUSIBLE_EPOCH = 1735689600;  // 2025-01-01
constexpr time_t NTP_MAX_PLAUSIBLE_EPOCH = 2524608000;  // 2050-01-01
// A gateway answers in single-digit ms and a public server in tens; past this the fast path is
// not going to work here and the fallback wants the remaining budget.
constexpr uint32_t QUICK_SNTP_TIMEOUT_MS = 1000;

bool quickSntpQuery(const IPAddress* targets, const size_t targetCount, time_t& outEpoch, IPAddress& outSource) {
  WiFiUDP udp;
  if (udp.begin(0) == 0) {
    LOG_DBG("CLK", "Quick SNTP: could not open a UDP socket");
    return false;
  }
  uint8_t packet[NTP_PACKET_SIZE] = {};
  packet[0] = 0x1B;  // LI=0, VN=3, Mode=3 (client)
  size_t sent = 0;
  for (size_t i = 0; i < targetCount; ++i) {
    if (static_cast<uint32_t>(targets[i]) == 0) continue;
    if (udp.beginPacket(targets[i], NTP_PORT) != 1) continue;
    udp.write(packet, NTP_PACKET_SIZE);
    if (udp.endPacket() == 1) sent++;
  }
  if (sent == 0) {
    udp.stop();
    return false;
  }
  const uint32_t deadline = millis() + QUICK_SNTP_TIMEOUT_MS;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int size = udp.parsePacket();
    if (size <= 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    uint8_t reply[NTP_PACKET_SIZE] = {};
    const int received = (size >= static_cast<int>(NTP_PACKET_SIZE)) ? udp.read(reply, NTP_PACKET_SIZE) : 0;
    // Nothing of that datagram may be left behind: parsePacket() refuses the next one while the
    // receive buffer still holds anything, and the good answer is often queued behind a bad one.
    udp.flush();
    if (received < static_cast<int>(NTP_PACKET_SIZE)) continue;
    const uint8_t leapIndicator = reply[0] >> 6;
    const uint8_t mode = reply[0] & 0x07;
    const uint8_t stratum = reply[1];
    // LI=3: the server's own clock is unsynchronised; mode 4 is a server reply, 5 a broadcast;
    // stratum 0 is a kiss-o'-death and 16+ unsynchronised. Arrived, but not to be believed.
    if (leapIndicator == 3 || (mode != 4 && mode != 5) || stratum == 0 || stratum > 15) continue;
    // Transmit timestamp, seconds part: bytes 40..43, big endian.
    const uint32_t ntpSeconds = (static_cast<uint32_t>(reply[40]) << 24) | (static_cast<uint32_t>(reply[41]) << 16) |
                                (static_cast<uint32_t>(reply[42]) << 8) | static_cast<uint32_t>(reply[43]);
    if (ntpSeconds <= NTP_TO_UNIX_OFFSET) continue;
    const time_t epoch = static_cast<time_t>(ntpSeconds - NTP_TO_UNIX_OFFSET);
    if (epoch < NTP_MIN_PLAUSIBLE_EPOCH || epoch > NTP_MAX_PLAUSIBLE_EPOCH) {
      LOG_DBG("CLK", "Quick SNTP: rejecting implausible epoch %lld", static_cast<long long>(epoch));
      continue;
    }
    outEpoch = epoch;
    outSource = udp.remoteIP();
    udp.stop();
    return true;
  }
  udp.stop();
  return false;
}
}  // namespace

bool HalClock::syncSystemClockFromNtp(const uint32_t timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  // Ask directly first (see quickSntpQuery); the full client below runs whenever this fails.
  {
    const unsigned long quickStartMs = millis();
    IPAddress targets[2];
    size_t count = 0;
    const IPAddress gateway = WiFi.gatewayIP();
    if (static_cast<uint32_t>(gateway) != 0) targets[count++] = gateway;
    targets[count++] = IPAddress(162, 159, 200, 1);  // time.cloudflare.com anycast, no DNS needed
    time_t epoch = 0;
    IPAddress source;
    if (quickSntpQuery(targets, count, epoch, source)) {
      timeval tv = {};
      tv.tv_sec = epoch;
      settimeofday(&tv, nullptr);
      LOG_INF("CLK", "Quick SNTP: %s answered in %lu ms", source.toString().c_str(), millis() - quickStartMs);
      return true;
    }
    LOG_DBG("CLK", "Quick SNTP: no usable answer after %lu ms; falling back to the SNTP client",
            millis() - quickStartMs);
  }

  // One sync at a time (see ntpSyncBusy). A second caller waits its turn for at
  // most its own timeout, then runs normally.
  const unsigned long claimStartMs = millis();
  while (!tryClaimNtpSync()) {
    if (millis() - claimStartMs >= timeoutMs) {
      LOG_ERR("CLK", "NTP sync busy on another task, giving up");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // esp-netif owns the SNTP client's state, runs every lwIP call on the TCP/IP
  // thread, and signals completion on a semaphore instead of being polled.
  // Costs one small calloc and one binary semaphore, both released by
  // esp_netif_sntp_deinit() below.
  esp_sntp_config_t config = {};
  config.wait_for_sync = true;
  config.start = true;
  config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;  // unused: renew_servers_after_new_IP stays false
  config.num_of_servers = 2;
  config.servers[0] = "pool.ntp.org";
  config.servers[1] = "time.nist.gov";

  const esp_err_t initResult = esp_netif_sntp_init(&config);
  if (initResult != ESP_OK) {
    LOG_ERR("CLK", "Failed to start NTP sync: %s", esp_err_to_name(initResult));
    releaseNtpSync();
    return false;
  }

  const esp_err_t syncResult = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeoutMs));

  // An SNTP timeout can leave the client's DNS lookup pending, with its callback
  // still registered in lwIP's DNS table. Arduino's hostByName() calls
  // dns_clear_cache() from the application task, which would run that callback
  // outside the core lock on the next hostname lookup (the sync or download
  // that follows). Drain it here, on the TCP/IP thread, before stopping SNTP.
  const esp_err_t clearResult = esp_netif_tcpip_exec(clearDnsCacheLocked, nullptr);
  esp_netif_sntp_deinit();
  releaseNtpSync();

  if (clearResult != ESP_OK) {
    LOG_ERR("CLK", "Failed to clear DNS after NTP sync: %s", esp_err_to_name(clearResult));
  }
  if (syncResult != ESP_OK) {
    LOG_ERR("CLK", "NTP sync failed: %s", esp_err_to_name(syncResult));
    return false;
  }
  return true;
}
