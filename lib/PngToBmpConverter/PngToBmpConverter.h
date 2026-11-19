#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool eightBit = false);

 public:
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true);
  // Same size as pngFileToBmpStream, but 8-bit grayscale: every tone kept, no
  // dithering (the 16-level sleep cover on the Picco quantizes it to its levels).
  static bool pngFileToGrayBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true);
  // crop=true fills the box (output can overflow it on one axis); false fits
  // inside it, preserving the aspect ratio.
  static bool pngFileToBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                         bool crop = true);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
};
