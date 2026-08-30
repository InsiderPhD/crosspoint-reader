#include "HeapReport.h"

#include <Arduino.h>
#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
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
  // Block counts: walks the heap's block list, allocates nothing itself.
  multi_heap_info_t info{};
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  s.allocatedBlocks = info.allocated_blocks;
  s.freeBlocks = info.free_blocks;
  s.bleEnabled = BluetoothHIDManager::getInstance().isEnabled();
  s.wifiMode = static_cast<uint8_t>(WiFi.getMode());
  return s;
}

uint32_t fragmentationPct(const Snapshot& s) {
  // How much of the free heap the single biggest block covers. A high free total
  // with a low ratio is the signature of the failures that look like "out of
  // memory" but are not.
  return s.freeHeap > 0 ? (s.largestBlock * 100U) / s.freeHeap : 0;
}

void logSnapshot(const char* tag, const Snapshot& s) {
  const uint32_t fragPct = fragmentationPct(s);

  LOG_INF("MEM", "===== RAM INVESTIGATOR [%s] =====", tag);
  LOG_INF("MEM", "heap     free=%u  min-ever=%u  total=%u", s.freeHeap, s.minEverFree, s.totalHeap);
  LOG_INF("MEM", "largest  block=%u  (%u%% of free -> %s)", s.largestBlock, fragPct,
          fragPct >= 60 ? "healthy" : (fragPct >= 30 ? "fragmenting" : "FRAGMENTED"));
  LOG_INF("MEM", "internal free=%u  largest=%u   dma free=%u", s.internalFree, s.internalLargest, s.dmaFree);
  LOG_INF("MEM", "blocks   allocated=%u  free=%u  (allocated is the fragmentation driver)", s.allocatedBlocks,
          s.freeBlocks);
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

void logBrief(const char* tag) {
  const Snapshot s = capture();
  LOG_INF("MEM", "[%-18s] free=%6u largest=%6u blocks=%u ble=%s", tag, s.freeHeap, s.largestBlock, s.allocatedBlocks,
          s.bleEnabled ? "on" : "off");
}

void dump(GfxRenderer& renderer) {
  // Capture first: everything below this line allocates, and under
  // EINK_DISPLAY_SINGLE_BUFFER_MODE the on-screen summary needs a 48KB scratch
  // buffer. Measuring after that would report the investigator's own footprint.
  const Snapshot s = capture();
  logSnapshot("chord", s);

  // Previous chord reading, so a second press shows what happened in between
  // (e.g. read at Home, open a book, read again -> the cost of the open).
  // Absolute numbers are what to compare across firmware builds; the delta is
  // for probing within one session.
  static Snapshot prev{};
  static bool havePrev = false;

  // On-screen summary is best-effort: when the heap is tight enough to be worth
  // investigating, storeBwBuffer() is exactly the allocation that fails. The
  // serial dump above has already landed either way.
  if (!renderer.storeBwBuffer()) {
    LOG_INF("MEM", "On-screen summary skipped: no heap for the scratch buffer (that IS the finding)");
    prev = s;
    havePrev = true;
    return;
  }

  const uint32_t fragPct = fragmentationPct(s);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int screenW = renderer.getScreenWidth();
  // Left-aligned label/value columns read far better than centred lines when
  // comparing two readings side by side. Keep the block itself centred.
  const int blockW = std::min(screenW - 40, 260);
  const int labelX = (screenW - blockW) / 2;
  const int valueX = labelX + blockW;

  int y = renderer.getScreenHeight() / 2 - lineH * 5;
  char line[48];

  auto row = [&](const char* label, const char* value) {
    renderer.drawText(UI_10_FONT_ID, labelX, y, label);
    // Right-align the value against the block's right edge.
    const int w = renderer.getTextWidth(UI_10_FONT_ID, value);
    renderer.drawText(UI_10_FONT_ID, valueX - w, y, value);
    y += lineH;
  };
  // Value plus a signed delta against the previous reading, when there is one.
  auto rowDelta = [&](const char* label, const uint32_t now, const uint32_t before) {
    if (havePrev) {
      snprintf(line, sizeof(line), "%u  (%+ld)", now, static_cast<long>(now) - static_cast<long>(before));
    } else {
      snprintf(line, sizeof(line), "%u", now);
    }
    row(label, line);
  };

  renderer.drawCenteredText(UI_10_FONT_ID, y, "RAM INVESTIGATOR");
  y += lineH * 2;

  rowDelta("Free", s.freeHeap, prev.freeHeap);
  rowDelta("Largest block", s.largestBlock, prev.largestBlock);

  // The verdict line: free bytes never distinguish exhausted from fragmented.
  snprintf(line, sizeof(line), "%u%% %s", fragPct,
           fragPct >= 60 ? "healthy" : (fragPct >= 30 ? "fragmenting" : "FRAGMENTED"));
  row("Fragmentation", line);

  // Allocated-block count: the number that moves when allocation *shape*
  // changes even if free bytes do not.
  rowDelta("Live blocks", s.allocatedBlocks, prev.allocatedBlocks);
  rowDelta("Free blocks", s.freeBlocks, prev.freeBlocks);
  rowDelta("Min ever free", s.minEverFree, prev.minEverFree);

  y += lineH / 2;
  // Headroom against the allocation that actually fails on this device, and the
  // radio state it depends on -- a reading is only comparable to another taken
  // with the same radios up.
  snprintf(line, sizeof(line), "32K inflate: %s", s.largestBlock >= 32768 ? "fits" : "WON'T FIT");
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineH;
  snprintf(line, sizeof(line), "BLE %s   WiFi mode %u", s.bleEnabled ? "ON" : "off", s.wifiMode);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineH;

  prev = s;
  havePrev = true;

  renderer.displayBuffer();
  delay(2000);
  renderer.restoreBwBuffer();
  renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
}

}  // namespace HeapReport
