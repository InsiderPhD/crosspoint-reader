#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;
class GrayLevelQuantizer;

// A picture's 8-bit gray straight into a 4 bpp level buffer (two pixels per
// byte, high nibble = left, panel-native rows), for panels that draw 16 levels
// (the Picco). Each logical pixel is mapped to the panel and given its level by
// an ordered dither, so a pixel written twice by a scaled decode keeps one
// level. Set as RenderConfig::gray16: the decoders then write only here (no
// framebuffer, no pixel cache, no 2-bit dither).
struct Gray16Sink {
  uint8_t* levels = nullptr;
  uint16_t panelWidth = 0, panelHeight = 0;
  const GrayLevelQuantizer* quantizer = nullptr;
  const GfxRenderer* renderer = nullptr;
  void write(int logicalX, int logicalY, uint8_t gray) const;
};

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;     // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;               // If non-empty, decoder will write pixel cache to this path
  const Gray16Sink* gray16 = nullptr;  // Non-null: 8-bit gray to this sink only (cachePath ignored)
  // 16-level planes (the X3): pixels are 4-bit ink levels (0 white .. 15 black)
  // instead of 2-bit tones, to the framebuffer writer and to a .px4 cache.
  bool levels4Bit = false;
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // Call from per-row/per-MCU decode callbacks (free functions, hence public):
  // yields one tick at most every 250 ms so multi-second decodes keep the idle
  // task (and its watchdog) fed. `lastYieldMs` is caller-held state,
  // initialized to the decode start time.
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // Validate decoder/header dimensions before narrowing them into the layout
  // representation. Shared by header probing and decoder fallbacks.
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format);

 protected:
  // Size validation helpers. The cap bounds decode TIME, not memory: both decoders
  // stream (JPEG in MCU bands at 1/2..1/8 coarse scale, PNG scanline-by-scanline
  // with its own width-based row-buffer guard), so RAM never scales with source
  // area. 8 MP admits real-world ebook art (KDP recommends 1600x2560 and
  // 2000x3000) while keeping a worst-case single decode in single-digit seconds;
  // the row callbacks yield periodically so a long decode cannot starve the idle
  // task's watchdog.
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;  // 8 MP (e.g. 2048 * 4096)

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
