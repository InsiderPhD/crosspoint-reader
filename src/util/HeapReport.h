#pragma once

#include <cstdint>

class GfxRenderer;

/**
 * On-demand heap investigator.
 *
 * Answers the question that actually matters on this device: is the heap
 * *exhausted* or merely *fragmented*? Free bytes alone never distinguish those,
 * and the fragmented case is what makes BLE enable, TLS handshakes and section
 * builds fail while `getFreeHeap()` still looks healthy.
 *
 * Triggered globally by POWER + Confirm (mirroring the POWER + DOWN screenshot
 * chord), so a reading can be taken on any screen without a build flag. Confirm
 * is resolved through MappedInputManager, so the chord follows the user's
 * front-button remapping.
 */
namespace HeapReport {

struct Snapshot {
  uint32_t freeHeap;
  uint32_t minEverFree;
  uint32_t largestBlock;
  uint32_t totalHeap;
  uint32_t internalFree;
  uint32_t internalLargest;
  uint32_t dmaFree;
  uint32_t stackHighWater;
  // Block counts, from heap_caps_get_info(). allocatedBlocks is the count that
  // matters when judging an allocation-shape change (e.g. collapsing several
  // per-object vectors into one arena): free bytes can be identical while the
  // number of live blocks — and so the fragmentation pressure — halves. Reading
  // it on the same page of the same book before and after a change is the most
  // direct evidence available on-device.
  uint32_t allocatedBlocks;
  uint32_t freeBlocks;
  bool bleEnabled;
  uint8_t wifiMode;
};

/** Percentage of free bytes covered by the single largest block (0 when free is 0). */
uint32_t fragmentationPct(const Snapshot& snap);

/**
 * Read the counters without allocating or rendering anything.
 * Always call this before any work that itself touches the heap.
 */
Snapshot capture();

/**
 * Write a snapshot plus the per-region map to serial.
 * `tag` labels the probe point, e.g. "chord", "ble.enable.before".
 */
void logSnapshot(const char* tag, const Snapshot& snap);

/** Convenience: capture() + logSnapshot() for probes in other code. */
void log(const char* tag);

/**
 * One-line probe: free / largest / live-block count, no region map.
 * Cheap enough to sprinkle at lifecycle transitions (boot, book open, section
 * build, BLE enable) so a single serial capture attributes where the heap went.
 * Use this for a ladder of readings; use log() when you need the region map.
 */
void logBrief(const char* tag);

/**
 * Full investigation: serial dump plus an on-screen summary so a reading can be
 * taken without a serial cable attached.
 */
void dump(GfxRenderer& renderer);

}  // namespace HeapReport
