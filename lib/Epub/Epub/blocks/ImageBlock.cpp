#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

// A page render draws each image up to ~13 times (BW double-refresh plus every
// grayscale band pass). An image the decoder cannot open fails that many times
// per page, each failure costing an SD open. Remember the failures so the first
// one is the only one paid for. Hashes, not paths, to keep this off the heap; a
// bounded array because the count is per page render.
//
// Cleared at the top of each page render (ImageBlock::clearRenderFailures), so a
// failure caused by transient pressure — the JPEG decoder losing its heap to
// Bluetooth, say — is retried on the next page instead of blanking that image
// for the rest of the boot.
constexpr size_t MAX_RENDER_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_RENDER_IMAGE_FAILURES];
size_t failedImageCount = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;  // FNV-1a
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisRender(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_RENDER_IMAGE_FAILURES || imageFailedThisRender(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

// Half-open image-local bounds: which rows and columns of the decoded image are
// actually going to land somewhere visible. Two clips combined —
//  - the logical screen, so an image hanging off an edge is trimmed rather than
//    rejected outright (the layout already reserved its full box);
//  - the active grayscale band, if any.
// Which axis the band constrains depends on orientation: in landscape a band is
// a range of image ROWS, in portrait a range of image COLUMNS (logical X maps to
// physical Y there). Clipping rows is what lets the caller skip their SD reads
// entirely; clipping columns only saves the unpack.
struct CachedImageClip {
  int x0, y0, x1, y1;
  bool empty() const { return x0 >= x1 || y0 >= y1; }
};

CachedImageClip cachedImageClip(const GfxRenderer& renderer, const int x, const int y, const int width,
                                const int height) {
  CachedImageClip clip{std::max(0, -x), std::max(0, -y), std::min(width, renderer.getScreenWidth() - x),
                       std::min(height, renderer.getScreenHeight() - y)};
  if (!renderer.isStripTargetActive()) return clip;

  const int bandTop = renderer.getWriteOriginY();
  const int bandBottom = bandTop + renderer.getWriteRows();
  const int panelH = renderer.getDisplayHeight();
  switch (renderer.getOrientation()) {
    case GfxRenderer::LandscapeCounterClockwise:
      clip.y0 = std::max(clip.y0, bandTop - y);
      clip.y1 = std::min(clip.y1, bandBottom - y);
      break;
    case GfxRenderer::LandscapeClockwise:
      clip.y0 = std::max(clip.y0, panelH - bandBottom - y);
      clip.y1 = std::min(clip.y1, panelH - bandTop - y);
      break;
    case GfxRenderer::Portrait:
      clip.x0 = std::max(clip.x0, panelH - bandBottom - x);
      clip.x1 = std::min(clip.x1, panelH - bandTop - x);
      break;
    case GfxRenderer::PortraitInverted:
      clip.x0 = std::max(clip.x0, bandTop - x);
      clip.x1 = std::min(clip.x1, bandBottom - x);
      break;
  }
  return clip;
}

// Reads and validates the 4-byte cache header, leaving the file positioned at
// the pixel payload. Shared by the render path and the cheap hasValidCache()
// probe so both agree on what counts as usable.
bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Allow 1 pixel tolerance for rounding differences between probe and decode.
  if (abs(cachedWidth - expectedWidth) > 1 || abs(cachedHeight - expectedHeight) > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Cached dimensions are the actual decoded size; clip against those.
  const auto clip = cachedImageClip(renderer, x, y, cachedWidth, cachedHeight);
  if (clip.empty()) return true;  // nothing of this image is in the visible band

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  const int rowsToRender = clip.y1 - clip.y0;
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsToRender) rowsPerRead = rowsToRender;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  // Skip straight to the first row that will actually be drawn. In landscape a
  // grayscale band covers only a slice of the image's rows, so this is where the
  // per-pass SD cost actually drops; in portrait clip.y0 is 0 unless the image
  // hangs off the top edge, and the saving is in the unpack loop instead.
  if (!cacheFile.seek(4 + (size_t)clip.y0 * bytesPerRow)) {
    LOG_ERR("IMG", "Cache seek error to row %d", clip.y0);
    free(readBuffer);
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = clip.y0; row < clip.y1; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (clip.y1 - row < rowsPerRead) ? (clip.y1 - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    pw.beginRow(y + row);
    for (int col = clip.x0; col < clip.x1; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", getCachePath(imagePath), cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisRender(imagePath) && !hasValidCache(); }

void ImageBlock::clearRenderFailures() { failedImageCount = 0; }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  // B/W only. In a grayscale plane pass the scratch is cleared to 0x00 and a
  // drawPixel(false) SETS the bit, i.e. marks that pixel for gray — so painting
  // the box's white interior here would fill it with a solid gray block instead
  // of leaving the B/W pass's outline alone.
  if (renderer.getRenderMode() != GfxRenderer::BW) return;

  // Clamp to the screen first. A partially offscreen image is now clipped rather
  // than dropped, and drawPixel logs an error for every out-of-range pixel — an
  // unclamped box hanging off an edge would emit thousands of them per pass.
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const auto fillClamped = [&](const int rx, const int ry, const int rw, const int rh, const bool state) {
    const int x0 = std::max(0, rx);
    const int y0 = std::max(0, ry);
    const int x1 = std::min(screenWidth, rx + rw);
    const int y1 = std::min(screenHeight, ry + rh);
    if (x0 >= x1 || y0 >= y1) return;
    renderer.fillRect(x0, y0, x1 - x0, y1 - y0, state);
  };

  fillClamped(x, y, width, height, true);
  if (width > 2 && height > 2) {
    fillClamped(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  // Images off (currently: while Bluetooth holds the heap the decoder needs).
  // Layout already reserved this space at build time, so the page is unchanged
  // apart from the image itself being blank.
  if (renderer.areImagesSuppressed()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image size: %dx%d", width, height);
    return;
  }

  // Reject only images that are entirely off-screen. A partially visible one is
  // clipped by the cache renderer and by the decoders' own screen clamping — it
  // used to be dropped whole, which lost the image over a single overhanging
  // pixel.
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }
  // A clipped decode would stream a .pxc that is missing its off-screen rows,
  // and the cache has no way to record that it is partial. Only cache a decode
  // that covers the whole image.
  const bool fullyOnScreen = x >= 0 && y >= 0 && x + width <= screenWidth && y + height <= screenHeight;

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  // Already known undecodable this page render - don't pay the failing open
  // again on every band pass. Draw the same outline box the pre-decode pass
  // used, so the reserved space reads as "image that didn't load" rather than
  // an unexplained gap.
  if (imageFailedThisRender(imagePath)) {
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  if (fullyOnScreen) {
    config.cachePath = cachePath;  // Enable caching during decode
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, w, h));
}
