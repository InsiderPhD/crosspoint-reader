#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// Read-only hardware facts for the About screen and the support export. Holds
// no settings, network state, credentials or book data. Every string points at
// flash or a static SDK/IDF table, so the snapshot owns nothing.
struct DeviceInfo {
  const char* boardName = "";          // active board profile, e.g. "Xteink X4"
  const char* chipModel = "";          // e.g. "ESP32-C3"
  const char* displayController = "";  // e.g. "SSD1677"
  const char* resetReason = "";        // ESP-IDF reset reason without its prefix, e.g. "POWERON"
  uint32_t flashBytes = 0;
  uint32_t psramBytes = 0;     // 0 on boards without PSRAM
  uint32_t uptimeSeconds = 0;  // since this boot (a deep-sleep wake is a boot)
  uint16_t cpuMHz = 0;
  uint16_t displayWidth = 0;  // native panel pixels, before orientation
  uint16_t displayHeight = 0;
  uint16_t chipRevision = 0;
  uint8_t resetReasonCode = 0;  // esp_reset_reason_t value
  uint8_t mac[6] = {};          // base WiFi station MAC, valid only when macValid
  bool macValid = false;
};

// Capture the facts above. Allocates nothing and does not touch the radio:
// the MAC comes from eFuse, so it works with WiFi and Bluetooth both down.
DeviceInfo getDeviceInfo();
}  // namespace HalSystem
