#pragma once

#include "ImageToFramebufferDecoder.h"

// Draws the first frame of a GIF (static GIFs, animated ones frozen on frame one) like the PNG and
// JPEG decoders, streaming the dithered result into the page's pixel cache. Interlaced GIFs are
// refused. Adapted from CrossPoint #2299.
class GifToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "GIF"; }
};
