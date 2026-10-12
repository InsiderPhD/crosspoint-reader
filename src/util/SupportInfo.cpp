#include "SupportInfo.h"

#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>

#include "CrossPointSettings.h"
#include "util/HeapReport.h"
#include "util/TimeUtils.h"

namespace {

// One formatted line straight to the file. 128 bytes covers the longest line
// below with room to spare and keeps the frame inside the stack budget; a
// longer line would be truncated by vsnprintf, never overrun.
bool writeLine(HalFile& file, const char* format, ...) __attribute__((format(printf, 2, 3)));
bool writeLine(HalFile& file, const char* format, ...) {
  char line[128];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length < 0) return false;
  if (length >= static_cast<int>(sizeof(line))) length = sizeof(line) - 1;
  return file.write(line, static_cast<size_t>(length)) == static_cast<size_t>(length);
}

const char* yesNo(const bool value) { return value ? "yes" : "no"; }

// "configured: yes/no" for an account store, from the file's existence alone.
// The file is never opened, so nothing in it can reach the report.
const char* configured(const char* path) { return yesNo(Storage.exists(path)); }

}  // namespace

SupportInfo::Snapshot SupportInfo::capture(const bool hasTouch) {
  // Heap first: reading it later would count this function's own callees.
  const HeapReport::Snapshot heap = HeapReport::capture();

  Snapshot s;
  s.heapFree = heap.freeHeap;
  s.heapLargestBlock = heap.largestBlock;
  s.heapMinFree = heap.minEverFree;

#ifdef SIMULATOR
  // The simulator's HAL mirror has no getDeviceInfo()/sdTotalBytes() yet (see
  // lib/hal/HalSystem.h and HalStorage.h for the real ones); stand-in values
  // keep the screen and the export testable on the desktop.
  s.boardName = "Simulator";
  s.chipModel = "host";
  s.displayController = "SDL";
  s.resetReason = "POWERON";
  s.resetReasonCode = 1;
  s.uptimeSeconds = static_cast<uint32_t>(millis() / 1000UL);
#else
  const HalSystem::DeviceInfo device = HalSystem::getDeviceInfo();
  s.boardName = device.boardName;
  s.chipModel = device.chipModel;
  s.displayController = device.displayController;
  s.resetReason = device.resetReason;
  s.flashBytes = device.flashBytes;
  s.psramBytes = device.psramBytes;
  s.uptimeSeconds = device.uptimeSeconds;
  s.cpuMHz = device.cpuMHz;
  s.displayWidth = device.displayWidth;
  s.displayHeight = device.displayHeight;
  s.chipRevision = device.chipRevision;
  s.resetReasonCode = device.resetReasonCode;
  for (size_t i = 0; i < sizeof(s.mac); i++) s.mac[i] = device.mac[i];
  s.macValid = device.macValid;
  s.sdTotalBytes = Storage.sdTotalBytes();
#endif

  const uint16_t battery = powerManager.getBatteryPercentage();
  s.batteryPercent = static_cast<uint8_t>(battery > 100 ? 100 : battery);
  s.hasTouch = hasTouch;
  s.hasFrontlight = halFrontlight.present();
  s.hasRtc = halClock.isAvailable();
  s.hasTiltSensor = halTiltSensor.isAvailable();
  return s;
}

void SupportInfo::captureSdUsage(Snapshot& snapshot) {
#ifndef SIMULATOR
  if (snapshot.sdTotalBytes > 0) {
    snapshot.sdUsedBytes = Storage.sdUsedBytes();
  }
#endif
  snapshot.sdUsageKnown = true;
}

bool SupportInfo::exportToSd(const Snapshot& s) {
  HalFile file;
  if (!Storage.openFileForWrite("SUPPORT", EXPORT_PATH, file)) {
    LOG_ERR("SUPPORT", "Cannot open %s for writing", EXPORT_PATH);
    return false;
  }

  bool ok = true;
  ok &= writeLine(file, "KatiePoint Reader support information\n");
  ok &= writeLine(file, "Contains no WiFi names or passwords, account details, tokens,\n");
  ok &= writeLine(file, "server addresses, MAC address, file names or book titles.\n\n");

  ok &= writeLine(file, "[firmware]\n");
  ok &= writeLine(file, "version: %s\n\n", CROSSPOINT_VERSION);

  ok &= writeLine(file, "[device]\n");
  ok &= writeLine(file, "board: %s\n", s.boardName);
  ok &= writeLine(file, "chip: %s rev %u\n", s.chipModel, static_cast<unsigned>(s.chipRevision));
  ok &= writeLine(file, "cpu_mhz: %u\n", static_cast<unsigned>(s.cpuMHz));
  ok &= writeLine(file, "flash_bytes: %lu\n", static_cast<unsigned long>(s.flashBytes));
  ok &= writeLine(file, "psram_bytes: %lu\n", static_cast<unsigned long>(s.psramBytes));
  ok &= writeLine(file, "display: %s %ux%u\n", s.displayController, static_cast<unsigned>(s.displayWidth),
                  static_cast<unsigned>(s.displayHeight));
  ok &= writeLine(file, "touch: %s\n", yesNo(s.hasTouch));
  ok &= writeLine(file, "frontlight: %s\n", yesNo(s.hasFrontlight));
  ok &= writeLine(file, "rtc: %s\n", yesNo(s.hasRtc));
  ok &= writeLine(file, "tilt_sensor: %s\n\n", yesNo(s.hasTiltSensor));

  ok &= writeLine(file, "[state]\n");
  ok &= writeLine(file, "uptime_seconds: %lu\n", static_cast<unsigned long>(s.uptimeSeconds));
  ok &= writeLine(file, "reset_reason: %s (%u)\n", s.resetReason, static_cast<unsigned>(s.resetReasonCode));
  ok &= writeLine(file, "battery_percent: %u\n", static_cast<unsigned>(s.batteryPercent));
  ok &= writeLine(file, "heap_free_bytes: %lu\n", static_cast<unsigned long>(s.heapFree));
  ok &= writeLine(file, "heap_largest_block_bytes: %lu\n", static_cast<unsigned long>(s.heapLargestBlock));
  ok &= writeLine(file, "heap_min_free_bytes: %lu\n", static_cast<unsigned long>(s.heapMinFree));
  ok &= writeLine(file, "sd_total_bytes: %llu\n", static_cast<unsigned long long>(s.sdTotalBytes));
  if (s.sdUsageKnown) {
    ok &= writeLine(file, "sd_used_bytes: %llu\n", static_cast<unsigned long long>(s.sdUsedBytes));
  }
  ok &= writeLine(file, "clock_set: %s\n\n", yesNo(TimeUtils::isClockValid()));

  // Allow-list of numeric settings that explain how the device renders and
  // behaves. Add to it deliberately: no free-text field belongs here (font
  // names, the "if found" contact, server URLs, user names).
  ok &= writeLine(file, "[settings]\n");
  ok &= writeLine(file, "ui_theme: %u\n", static_cast<unsigned>(SETTINGS.uiTheme));
  ok &= writeLine(file, "dark_mode: %u\n", static_cast<unsigned>(SETTINGS.darkMode));
  ok &= writeLine(file, "orientation: %u\n", static_cast<unsigned>(SETTINGS.orientation));
  ok &= writeLine(file, "font_family: %u\n", static_cast<unsigned>(SETTINGS.fontFamily));
  ok &= writeLine(file, "custom_font: %s\n", yesNo(SETTINGS.sdFontFamilyName[0] != '\0'));
  ok &= writeLine(file, "font_size: %u\n", static_cast<unsigned>(SETTINGS.fontSize));
  ok &= writeLine(file, "line_spacing: %u\n", static_cast<unsigned>(SETTINGS.lineSpacing));
  ok &= writeLine(file, "paragraph_alignment: %u\n", static_cast<unsigned>(SETTINGS.paragraphAlignment));
  ok &= writeLine(file, "screen_margin: %u\n", static_cast<unsigned>(SETTINGS.screenMargin));
  ok &= writeLine(file, "hyphenation: %u\n", static_cast<unsigned>(SETTINGS.hyphenationEnabled));
  ok &= writeLine(file, "embedded_style: %u\n", static_cast<unsigned>(SETTINGS.embeddedStyle));
  ok &= writeLine(file, "bionic_reading: %u\n", static_cast<unsigned>(SETTINGS.bionicReading));
  ok &= writeLine(file, "text_anti_aliasing: %u\n", static_cast<unsigned>(SETTINGS.textAntiAliasing));
  ok &= writeLine(file, "refresh_frequency: %u\n", static_cast<unsigned>(SETTINGS.refreshFrequency));
  ok &= writeLine(file, "sleep_screen: %u\n", static_cast<unsigned>(SETTINGS.sleepScreen));
  ok &= writeLine(file, "sleep_timeout: %u\n", static_cast<unsigned>(SETTINGS.sleepTimeout));
  ok &= writeLine(file, "clock_timezone: %u\n", static_cast<unsigned>(SETTINGS.clockTimezone));
  ok &= writeLine(file, "clock_dst: %u\n", static_cast<unsigned>(SETTINGS.clockDst));
  ok &= writeLine(file, "dev_mode: %u\n\n", static_cast<unsigned>(SETTINGS.devMode));

  ok &= writeLine(file, "[accounts]\n");
  ok &= writeLine(file, "wifi_configured: %s\n", configured("/.crosspoint/wifi.json"));
  ok &= writeLine(file, "bookfusion_configured: %s\n", configured("/.crosspoint/bookfusion.json"));
  ok &= writeLine(file, "koreader_configured: %s\n", configured("/.crosspoint/koreader.json"));
  ok &= writeLine(file, "hardcover_configured: %s\n", configured("/.crosspoint/hardcover.json"));

  ok &= file.close();
  if (!ok) {
    LOG_ERR("SUPPORT", "Short write to %s", EXPORT_PATH);
  }
  return ok;
}
