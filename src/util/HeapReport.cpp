#include "HeapReport.h"

#include <Arduino.h>
#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <cstdio>

#include "fontIds.h"

namespace HeapReport {

Snapshot capture() {
  Snapshot s{};
  s.freeHeap = ESP.getFreeHeap();
  s.minEverFree = ESP.getMinFreeHeap();
  s.largestBlock = ESP.getMaxAllocHeap();
  s.totalHeap = ESP.getHeapSize();
  s.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s.dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
  // In words on this port; report bytes so it lines up with everything else.
  s.stackHighWater = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
  s.bleEnabled = BluetoothHIDManager::getInstance().isEnabled();
  s.wifiMode = static_cast<uint8_t>(WiFi.getMode());
  return s;
}

void logSnapshot(const char* tag, const Snapshot& s) {
  // Fragmentation ratio: how much of the free heap the single biggest block
  // covers. A high free total with a low ratio is the signature of the failures
  // that look like "out of memory" but are not.
  const uint32_t fragPct = s.freeHeap > 0 ? (s.largestBlock * 100U) / s.freeHeap : 0;

  LOG_INF("MEM", "===== RAM INVESTIGATOR [%s] =====", tag);
  LOG_INF("MEM", "heap     free=%u  min-ever=%u  total=%u", s.freeHeap, s.minEverFree, s.totalHeap);
  LOG_INF("MEM", "largest  block=%u  (%u%% of free -> %s)", s.largestBlock, fragPct,
          fragPct >= 60 ? "healthy" : (fragPct >= 30 ? "fragmenting" : "FRAGMENTED"));
  LOG_INF("MEM", "internal free=%u  largest=%u   dma free=%u", s.internalFree, s.internalLargest, s.dmaFree);
  LOG_INF("MEM", "stack    high-water=%u bytes free on this task", s.stackHighWater);
  LOG_INF("MEM", "radios   ble=%s  wifi-mode=%u", s.bleEnabled ? "ON" : "off", s.wifiMode);

  // Headroom against the three allocations that actually fail on this device.
  LOG_INF("MEM", "fits?    BLE-ctrl(30K)=%s  TLS-rec(2x16K)=%s  inflate(32K)=%s",
          s.largestBlock >= 30720 ? "yes" : "NO", s.largestBlock >= 16384 ? "yes" : "NO",
          s.largestBlock >= 32768 ? "yes" : "NO");

  // Region map: which pool is fragmented, and how many free blocks it is cut
  // into. Goes to stdout (USB CDC) rather than through the LOG macros.
  heap_caps_print_heap_info(MALLOC_CAP_8BIT);
  LOG_INF("MEM", "===== end [%s] =====", tag);
}

void log(const char* tag) { logSnapshot(tag, capture()); }

void dump(GfxRenderer& renderer) {
  // Capture first: everything below this line allocates, and under
  // EINK_DISPLAY_SINGLE_BUFFER_MODE the on-screen summary needs a 48KB scratch
  // buffer. Measuring after that would report the investigator's own footprint.
  const Snapshot s = capture();
  logSnapshot("chord", s);

  // On-screen summary is best-effort: when the heap is tight enough to be worth
  // investigating, storeBwBuffer() is exactly the allocation that fails. The
  // serial dump above has already landed either way.
  if (!renderer.storeBwBuffer()) {
    LOG_INF("MEM", "On-screen summary skipped: no heap for the scratch buffer (that IS the finding)");
    return;
  }

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  int y = renderer.getScreenHeight() / 2 - lineH * 3;
  char line[64];

  renderer.drawCenteredText(UI_10_FONT_ID, y, "RAM INVESTIGATOR");
  y += lineH * 2;

  snprintf(line, sizeof(line), "Free      %u", s.freeHeap);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineH;

  snprintf(line, sizeof(line), "Largest   %u", s.largestBlock);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineH;

  snprintf(line, sizeof(line), "Min ever  %u", s.minEverFree);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineH;

  snprintf(line, sizeof(line), "BLE %s", s.bleEnabled ? "on" : "off");
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);

  renderer.displayBuffer();
  delay(2000);
  renderer.restoreBwBuffer();
  renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
}

}  // namespace HeapReport
