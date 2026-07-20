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
 * Triggered globally by POWER + UP (mirroring the POWER + DOWN screenshot
 * chord), so a reading can be taken on any screen without a build flag.
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
  bool bleEnabled;
  uint8_t wifiMode;
};

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
 * Full investigation: serial dump plus an on-screen summary so a reading can be
 * taken without a serial cable attached.
 */
void dump(GfxRenderer& renderer);

}  // namespace HeapReport
