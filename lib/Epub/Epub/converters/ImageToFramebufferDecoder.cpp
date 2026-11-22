#include "ImageToFramebufferDecoder.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <GrayLevelQuantizer.h>
#include <Logging.h>

bool ImageToFramebufferDecoder::validateAndStoreDimensions(const int64_t width, const int64_t height,
                                                           ImageDimensions& out, const char* format) {
  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid %s dimensions: %lldx%lld", format, static_cast<long long>(width),
            static_cast<long long>(height));
    return false;
  }
  // Checked before multiplying: the layout representation is int16_t, and an
  // oversized field would otherwise narrow into a plausible-looking small value.
  if (width > MAX_SOURCE_DIMENSION || height > MAX_SOURCE_DIMENSION) {
    LOG_ERR("IMG", "%s dimensions exceed supported limit: %lldx%lld (max %lld per dimension)", format,
            static_cast<long long>(width), static_cast<long long>(height),
            static_cast<long long>(MAX_SOURCE_DIMENSION));
    return false;
  }

  const int64_t pixels = width * height;
  if (pixels > MAX_SOURCE_PIXELS) {
    LOG_ERR("IMG", "%s too large (%lldx%lld = %lld pixels), max supported: %lld pixels", format,
            static_cast<long long>(width), static_cast<long long>(height), static_cast<long long>(pixels),
            static_cast<long long>(MAX_SOURCE_PIXELS));
    return false;
  }

  out.width = static_cast<int16_t>(width);
  out.height = static_cast<int16_t>(height);
  return true;
}

void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t& lastYieldMs) {
  const uint32_t now = millis();
  if (now - lastYieldMs >= 250) {
    lastYieldMs = now;
    vTaskDelay(1);
  }
}

void ImageToFramebufferDecoder::warnUnsupportedFeature(const std::string& feature, const std::string& imagePath) {
  LOG_ERR("IMG", "Warning: Unsupported feature '%s' in image '%s'. Image may not display correctly.", feature.c_str(),
          imagePath.c_str());
}

void Gray16Sink::write(const int logicalX, const int logicalY, const uint8_t gray) const {
  int px, py;
  renderer->logicalToPanel(logicalX, logicalY, &px, &py);
  if (px < 0 || py < 0 || px >= panelWidth || py >= panelHeight) return;
  const uint8_t level = quantizer->ink(gray, static_cast<unsigned>(px), static_cast<unsigned>(py));
  uint8_t& b = levels[static_cast<size_t>(py) * (panelWidth / 2) + px / 2];
  b = (px & 1) ? static_cast<uint8_t>((b & 0xF0) | level) : static_cast<uint8_t>((b & 0x0F) | level << 4);
}
