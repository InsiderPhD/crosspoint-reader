#include "DownloadFailureLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <sdkconfig.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "util/TimeUtils.h"

namespace {

constexpr const char* kOldPath = "/.crosspoint/download_failure.log.old";

// Appends `text` (no newline handling of its own) to the log file. Opening in
// append mode per call keeps no handle across the transfer.
void appendRaw(const char* text) {
  Storage.ensureDirectoryExists("/.crosspoint");
  FsFile file = Storage.open(DownloadFailureLog::kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("DLLOG", "Cannot open %s", DownloadFailureLog::kPath);
    return;
  }
  file.write(text, strlen(text));
  file.close();
}

void rotateIfLarge() {
  size_t size = 0;
  {
    FsFile file = Storage.open(DownloadFailureLog::kPath, O_RDONLY);
    if (!file) return;
    size = file.fileSize();
    file.close();
  }
  if (size < DownloadFailureLog::kMaxBytes) return;
  Storage.remove(kOldPath);
  Storage.rename(DownloadFailureLog::kPath, kOldPath);
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SHIELD:
      return "NO_SHIELD";
    default:
      return "OTHER";
  }
}

}  // namespace

namespace DownloadFailureLog {

void line(const char* fmt, ...) {
  char buf[192];
  const int prefix = snprintf(buf, sizeof(buf), "[+%lums] ", static_cast<unsigned long>(millis()));
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf + prefix, sizeof(buf) - prefix - 1, fmt, args);
  va_end(args);
  strlcat(buf, "\n", sizeof(buf));
  appendRaw(buf);
  LOG_DBG("DLLOG", "%s", buf + prefix);
}

void section(const char* title) {
  rotateIfLarge();
  char when[32] = "clock not set";
  if (TimeUtils::isClockValid()) {
    strlcpy(when, TimeUtils::formatDateTime(TimeUtils::getCurrentValidTimestamp()).c_str(), sizeof(when));
  }
  line("==== %s | %s | fw %s | target %s ====", title, when, CROSSPOINT_VERSION, CONFIG_IDF_TARGET);
}

void environment(const char* label) {
  line("%s: heap free=%u largest=%u min-ever=%u; task stack free=%u", label, (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT), (unsigned)ESP.getMinFreeHeap(),
       (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  const wl_status_t status = WiFi.status();
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  if (status == WL_CONNECTED) {
    line("%s: wifi %s ssid=\"%s\" rssi=%d ch=%d ps=%d ip=%s gw=%s dns=%s", label, wifiStatusName(status),
         WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.channel(), static_cast<int>(ps), WiFi.localIP().toString().c_str(),
         WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  } else {
    line("%s: wifi %s (status %d) ps=%d", label, wifiStatusName(status), static_cast<int>(status),
         static_cast<int>(ps));
  }
}

void url(const char* label, const char* fullUrl) {
  // Copy up to the '?' verbatim, then only parameter names: "a=&b=&c=".
  char buf[160];
  size_t out = 0;
  const char* p = fullUrl;
  for (; *p != '\0' && *p != '?' && out + 1 < sizeof(buf); ++p) buf[out++] = *p;
  if (*p == '?') {
    bool inValue = false;
    for (; *p != '\0' && out + 1 < sizeof(buf); ++p) {
      if (*p == '&' || *p == '?') {
        inValue = false;
        buf[out++] = *p;
      } else if (*p == '=') {
        inValue = true;
        buf[out++] = *p;
      } else if (!inValue) {
        buf[out++] = *p;
      }
    }
  }
  buf[out] = '\0';
  line("%s: %s (len %u%s)", label, buf, (unsigned)strlen(fullUrl), *p != '\0' ? ", redacted+truncated" : "");
}

}  // namespace DownloadFailureLog
