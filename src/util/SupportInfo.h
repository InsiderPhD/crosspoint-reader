#pragma once

#include <cstdint>

// Device diagnostics behind Settings > System > About, and the support file
// that screen can save to the SD card.
//
// Everything here is read on demand and held in one plain struct owned by the
// caller (the About screen keeps it as a member, so it lives and dies with that
// screen). No heap allocation, nothing resident outside the screen.
namespace SupportInfo {

// Where exportToSd() writes. Fixed name in the SD root so it is easy to find
// and each export replaces the previous one.
inline constexpr char EXPORT_PATH[] = "/support-info.txt";

struct Snapshot {
  // Hardware (HalSystem::getDeviceInfo). Strings point at flash/static tables.
  const char* boardName = "";
  const char* chipModel = "";
  const char* displayController = "";
  const char* resetReason = "";
  uint32_t flashBytes = 0;
  uint32_t psramBytes = 0;
  uint32_t uptimeSeconds = 0;
  uint16_t cpuMHz = 0;
  uint16_t displayWidth = 0;
  uint16_t displayHeight = 0;
  uint16_t chipRevision = 0;
  uint8_t resetReasonCode = 0;
  uint8_t mac[6] = {};
  bool macValid = false;

  // Heap, read before anything else so the numbers are not skewed by the
  // screen's own transient strings.
  uint32_t heapFree = 0;
  uint32_t heapLargestBlock = 0;
  uint32_t heapMinFree = 0;

  // SD card. sdUsedBytes is only meaningful once sdUsageKnown is set by
  // captureSdUsage() — it is slow, so capture() never computes it.
  uint64_t sdTotalBytes = 0;
  uint64_t sdUsedBytes = 0;
  bool sdUsageKnown = false;

  uint8_t batteryPercent = 0;
  bool hasTouch = false;
  bool hasFrontlight = false;
  bool hasRtc = false;
  bool hasTiltSensor = false;
};

// Fast: no SD traffic beyond a cached size, no radio, no allocation.
Snapshot capture(bool hasTouch);

// Fill in SD usage. Walks the card's allocation table, which can take a few
// seconds on a large card, so call it only after the screen has drawn.
void captureSdUsage(Snapshot& snapshot);

// Write a plain-text support report to EXPORT_PATH. Privacy-safe by
// construction: it contains hardware facts, heap/SD/battery readings, an
// allow-list of numeric display/reader settings and three lifetime reading
// totals (books finished, reading time, sessions). It never contains WiFi names
// or passwords, account names, tokens, server URLs, the MAC address, file
// names or book titles — accounts appear only as "configured: yes/no".
// Returns false (after logging) when the file cannot be written.
bool exportToSd(const Snapshot& snapshot);

}  // namespace SupportInfo
