#pragma once

#include <HalStorage.h>

class Print;

// First frame of a GIF cover -> BMP (2-bit cover, 1-bit thumbnail), in the same shapes the JPEG
// and PNG converters write. Adapted from CrossPoint #2299. Interlaced GIFs are refused.
class GifToBmpConverter {
  static bool gifFileToBmpStreamInternal(FsFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true);

 public:
  // The decoder is one ~26 KB block (AnimatedGIF, MAX_WIDTH 3072). False when the heap cannot
  // hold it right now: a transient condition, so callers should not cache the failure.
  static bool hasHeapForDecoder();

  static bool gifFileToBmpStream(FsFile& gifFile, Print& bmpOut, bool crop = true);
  static bool gifFileTo1BitBmpStreamWithSize(FsFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
