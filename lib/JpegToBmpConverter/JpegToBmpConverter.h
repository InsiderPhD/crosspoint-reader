#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

class Print;
class ZipFile;

// Lends the converter a caller-owned buffer for its two large allocations: the
// ~20KB JPEGDEC decoder object and the MCU row buffer (16 x srcWidth bytes).
// Mirrors InflateScratchLease — on this device the heap's largest free block is
// often capped well below the decoder's needs (BLE fragmentation), while the
// 48KB e-ink framebuffer is contiguous by construction and idle during a
// conversion. With a lease active the pre-flight heap guard drops to what the
// small remaining allocations (row/scaling/dither buffers) actually need.
//
// CALLER CONTRACT: nothing may render for the lifetime of the lease. The lease
// destroys the buffer's contents, so the caller must clear/repaint after.
class JpegScratchLease {
 public:
  JpegScratchLease(uint8_t* buffer, size_t length);
  ~JpegScratchLease();

  JpegScratchLease(const JpegScratchLease&) = delete;
  JpegScratchLease& operator=(const JpegScratchLease&) = delete;

  // False when the buffer was too small or another lease is already active;
  // the converter then falls back to malloc + the conservative heap guard.
  bool active() const { return registered; }

 private:
  bool registered = false;
};

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true);

 public:
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
