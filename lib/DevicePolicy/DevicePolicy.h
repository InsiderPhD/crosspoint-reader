#pragma once

// --- Device-class policy ------------------------------------------------------
//
// A handful of behaviours in this firmware exist purely because the ESP32-C3
// devices (X3/X4) run out of RAM and radio at the same time: ~380KB total, no
// PSRAM, one 48KB framebuffer, and a BT controller that must not come up near a
// WiFi session. They are workarounds with real costs, and on a board with room
// (X4 Pro: ESP32-S3, 512KB internal SRAM + 8MB PSRAM, hardware/SW radio coex)
// they cost without buying anything.
//
// This header names the device class once so those behaviours can be switched
// off in one place instead of sprinkling FREEINK_DEVICE_X4PRO through the
// firmware. Keyed on PSRAM presence rather than on the device macro so any
// future PSRAM-equipped board inherits the same treatment.
#ifndef CROSSPOINT_TIGHT_HEAP
#if defined(BOARD_HAS_PSRAM)
#define CROSSPOINT_TIGHT_HEAP 0
#else
#define CROSSPOINT_TIGHT_HEAP 1
#endif
#endif

// --- Framebuffer-scratch borrow ----------------------------------------------
//
// Three subsystems cannibalise the idle e-ink framebuffer as scratch RAM:
//   * InflateScratchLease   — 32KB deflate dictionary (lib/InflateReader)
//   * JpegScratchLease      — ~20KB JPEGDEC object + MCU rows (lib/JpegToBmpConverter)
//   * TlsFramebufferBorrow  — wolfSSL session + record buffers (src/activities/reader)
//
// They exist because free bytes are not the constraint on the C3 — contiguity
// is. Once the BLE stack has run, the largest free block is capped around 25KB
// for the rest of the session, so a 32KB request fails no matter how many bytes
// are free. The framebuffer is allocated once at boot, is permanently
// contiguous, and e-ink holds its image with no RAM, so it can be lent out and
// repainted afterwards.
//
// The cost is that the framebuffer ends up holding garbage and every caller must
// fully repaint. Any path that instead paints partially shows that garbage as
// on-screen artefacts — which is exactly what WiFi transfers looked like on the
// X4 Pro, where the borrow was never needed in the first place.
#ifndef CROSSPOINT_FB_SCRATCH_BORROW
#define CROSSPOINT_FB_SCRATCH_BORROW CROSSPOINT_TIGHT_HEAP
#endif

// --- BLE exclusivity ---------------------------------------------------------
//
// On the C3 the BLE stack (~56KB, needing a >=30KB contiguous block) cannot
// share the device with WiFi or with a memory-critical operation:
//   * enable() powers WiFi down first (mutual exclusion),
//   * BleMemoryPause tears the stack down for section builds / syncs / a
//     degraded-heap render, leaving the remote to reconnect afterwards,
//   * auto-restore waits out BLE_WIFI_SETTLE_MS because bringing the BT
//     controller up soon after esp_wifi_stop() hard-freezes that chip,
//   * book images are suppressed whenever Bluetooth is in use, because the JPEG
//     decoder's ~36KB fragments the heap into the zone where a later controller
//     init hangs.
//
// None of that applies to a PSRAM board with software coexistence compiled in
// (CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y in the S3 framework libs): BLE can stay
// up through WiFi sessions, chapter builds and image decodes, so the remote
// keeps working instead of dropping its link several times per book.
#ifndef CROSSPOINT_BLE_EXCLUSIVE
#define CROSSPOINT_BLE_EXCLUSIVE CROSSPOINT_TIGHT_HEAP
#endif

// --- Download write coalescing ----------------------------------------------
//
// Bytes to buffer before writing a download to SD, or 0 to write each transport
// chunk straight through. The TLS transport delivers the body in 2KB pieces (a
// stack buffer in SecureHttpClient, sized for an 8KB loop-task stack), and on
// the X4 Pro every SD write additionally bounces through a DMA-capable malloc +
// memcpy in the SDMMC block device, so writing at 2KB granularity pays that per
// 2KB. Coalescing into 32KB writes turns ~16 short SDMMC transactions into one
// multi-sector run.
//
// Not enabled on the C3: there the buffer would have to come out of the same
// heap the TLS session is already struggling to find a block in. On a PSRAM
// board the allocation is >=4KB, so CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
// places it in PSRAM and it costs no internal DRAM at all.
#ifndef CROSSPOINT_DOWNLOAD_WRITE_BUFFER
#if CROSSPOINT_TIGHT_HEAP
#define CROSSPOINT_DOWNLOAD_WRITE_BUFFER 0
#else
#define CROSSPOINT_DOWNLOAD_WRITE_BUFFER (32 * 1024)
#endif
#endif
