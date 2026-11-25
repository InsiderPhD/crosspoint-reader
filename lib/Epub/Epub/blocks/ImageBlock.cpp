#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <PrintSerialization.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - row-major, packed: .pxc 2 bits per pixel (4 per byte, MSB first,
//   0 black .. 3 white); .px4 4 bits per pixel (2 per byte, high nibble first, the ink
//   level 0 white .. 15 black the 16-level planes draw from)

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
int cacheBytesPerRow(const int width, const bool levels4) { return levels4 ? (width + 1) / 2 : (width + 3) / 4; }

// One cached pixel of a row: a 2-bit tone or, with levels4, a 4-bit ink level.
inline uint8_t cachePixelAt(const uint8_t* rowBuffer, const int col, const bool levels4) {
  if (levels4) return (col & 1) ? (rowBuffer[col >> 1] & 0x0F) : (rowBuffer[col >> 1] >> 4);
  return (rowBuffer[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;  // MSB first within byte
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight, const bool levels4) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  // Allow 1 pixel tolerance for rounding differences between probe and decode.
  if (abs(cachedWidth - expectedWidth) > 1 || abs(cachedHeight - expectedHeight) > 1) {
    return false;
  }

  const size_t bytesPerRow = cacheBytesPerRow(cachedWidth, levels4);
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Replace the extension with .pxc (2-bit pixel cache) or .px4 (4-bit, the 16-level planes').
std::string getCachePath(const std::string& imagePath, const bool levels4) {
  const char* const ext = levels4 ? ".px4" : ".pxc";
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ext;
  }
  return imagePath + ext;
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// An image page draws its image ~13 times (the BW pass plus every band of both
// gray planes) and each pass streamed the whole .pxc off SD again — ~100ms per
// pass for a full-page image, and the dominant cost of showing one. The first
// pass now pulls the payload into RAM and the rest render from it.
//
// Chunked: a single ~84KB block (the full-viewport worst case, 2bpp) rarely
// survives a fragmented mid-render heap, whereas 16KB pieces usually do. Every
// chunk is heap-gated and any failure falls back to the streaming path below,
// unchanged. The reader releases the slot when the page render completes, so
// nothing stays resident across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB, comfortably above the ~84 KB worst case
// Headroom to leave for the rest of the page render once the slot is taken. The
// grayscale strip scratch (gwBytes * 80 = 8KB, with smaller degraded fallbacks)
// is the only sizeable allocation still to come. Decoders are deliberately NOT
// in this budget: the reader disables the slot for any page that still has an
// image to decode, so a 36KB JPEG or 60KB PNG decode never competes with it.
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary and are reassembled into a stack buffer.
// (800 + 3) / 4 = 201 for the widest panel, so this stays under the 256-byte
// stack-local limit. A 4-bit .px4 row is twice as long, so only pictures up to
// 416 px wide take the slot in the 16-level planes; wider ones stream.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;
bool pxcSlot4Bit = false;  // the slot holds a .px4 payload
// Cleared by the reader on a page that still has an image to decode, so the
// slot never takes heap the decoder is about to need.
bool pxcSlotAllowed = true;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
  pxcSlot4Bit = false;
}

const uint8_t* pxcRowPtr(const size_t rowStart, const int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile must be positioned just past the header. True when the slot holds
// the whole pixel payload for this cache path afterwards.
bool loadPxcSlot(const uint64_t cacheHash, HalFile& cacheFile, const uint16_t cachedWidth, const uint16_t cachedHeight,
                 const int bytesPerRow, const bool levels4) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) return false;

  size_t remaining = static_cast<size_t>(bytesPerRow) * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) return false;

  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    if (ESP.getFreeHeap() < remaining + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      releasePxcSlot();
      return false;
    }
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i] || cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  pxcSlot4Bit = levels4;
  return true;
}

void renderFromPxcSlot(GfxRenderer& renderer, const CachedImageClip& clip, const int x, const int y) {
  const bool levels4 = pxcSlot4Bit;
  const int bytesPerRow = cacheBytesPerRow(pxcSlotWidth, levels4);
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);
  pw.levels4Bit = levels4;

  for (int row = clip.y0; row < clip.y1; row++) {
    const uint8_t* rowBuffer = pxcRowPtr(static_cast<size_t>(row) * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    for (int col = clip.x0; col < clip.x1; col++) {
      pw.writePixel(x + col, cachePixelAt(rowBuffer, col, levels4));
    }
  }
}

bool renderFromCacheFile(GfxRenderer& renderer, const std::string& cachePath, uint64_t cacheHash, int x, int y,
                         int expectedWidth, int expectedHeight, bool levels4);

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight, const bool levels4) {
  // A later pass of the same page render: the payload is already in RAM, so
  // skip the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    const auto clip = cachedImageClip(renderer, x, y, pxcSlotWidth, pxcSlotHeight);
    if (!clip.empty()) renderFromPxcSlot(renderer, clip, x, y);
    return true;
  }

  return renderFromCacheFile(renderer, cachePath, cacheHash, x, y, expectedWidth, expectedHeight, levels4);
}

bool renderFromCacheFile(GfxRenderer& renderer, const std::string& cachePath, const uint64_t cacheHash, int x, int y,
                         int expectedWidth, int expectedHeight, const bool levels4) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight, levels4)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Cached dimensions are the actual decoded size; clip against those.
  const auto clip = cachedImageClip(renderer, x, y, cachedWidth, cachedHeight);

  const int bytesPerRowForSlot = cacheBytesPerRow(cachedWidth, levels4);

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Attempted even when this pass is
  // fully clipped out, because a later band will want it.
  //
  // Only an EMPTY slot is claimed. The slot lives until the page render
  // completes, so a populated slot with a different hash means another image on
  // this page owns it; evicting would make a two-image page reload both from SD
  // on every pass — all the SD traffic of streaming plus the allocation churn.
  // Later images simply take the streaming path, exactly as before.
  if (pxcSlotAllowed && pxcSlotHash == 0 &&
      loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRowForSlot, levels4)) {
    LOG_DBG("IMG", "Cached payload now in RAM: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);
    if (!clip.empty()) renderFromPxcSlot(renderer, clip, x, y);
    return true;
  }

  if (clip.empty()) return true;  // nothing of this image is in the visible band

  // No rewind needed after a failed slot load: the streaming path below seeks to
  // an absolute offset, so wherever the partial read left the handle is moot.
  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = cacheBytesPerRow(cachedWidth, levels4);
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
  pw.levels4Bit = levels4;

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
      pw.writePixel(x + col, cachePixelAt(rowBuffer, col, levels4));
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache(const bool levels4) const {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", getCachePath(imagePath, levels4), cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight, levels4);
}

bool ImageBlock::needsDecode(const bool levels4) const {
  return !imageFailedThisRender(imagePath) && !hasValidCache(levels4);
}

void ImageBlock::clearRenderFailures() { failedImageCount = 0; }

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::setRenderCacheAllowed(const bool allowed) {
  pxcSlotAllowed = allowed;
  if (!allowed) releasePxcSlot();
}

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
  // 16-level plane passes on a board with a page buffer (Picco): the picture is
  // painted afterwards in 8 bits (renderGray16); a 2-bit draw here would only
  // be overwritten, and an uncached one would cost a decode per plane. The X3
  // has no such buffer: every plane draws the picture's 4-bit ink levels from
  // its .px4 cache (decoded once, by the first plane that finds none).
  if (renderer.getRenderMode() == GfxRenderer::GRAY16_PLANE && renderer.gray16ImagesDeferred()) return;
  const bool levels4 = renderer.getRenderMode() == GfxRenderer::GRAY16_PLANE;

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
  std::string cachePath = getCachePath(imagePath, levels4);
  if (renderFromCache(renderer, cachePath, x, y, width, height, levels4)) {
    renderer.preserveImagePolarity(x, y, width, height);
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
  config.levels4Bit = levels4;
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

  renderer.preserveImagePolarity(x, y, width, height);
  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::renderGray16(GfxRenderer& renderer, const int x, const int y, const Gray16Sink& sink) const {
  if (renderer.areImagesSuppressed() || width <= 0 || height <= 0 || imageFailedThisRender(imagePath)) return false;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) return false;
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) return false;
  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = false;
  config.useExactDimensions = true;  // the size the page was laid out with
  config.gray16 = &sink;             // no framebuffer writes, no cache
  if (!decoder->decodeToFramebuffer(imagePath, renderer, config)) {
    LOG_ERR("IMG", "16-level decode failed: %s", imagePath.c_str());
    return false;
  }
  return true;
}

bool ImageBlock::serialize(Print& file) {
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
