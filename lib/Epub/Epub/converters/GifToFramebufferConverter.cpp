#include "GifToFramebufferConverter.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <GifCommon.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

// Last: AnimatedGIF.h defines a MAX_WIDTH macro, which must not reach other headers.
#include <AnimatedGIF.h>

static_assert(MAX_WIDTH == CROSSPOINT_GIF_MAX_WIDTH, "AnimatedGIF not patched: run scripts/patch_animatedgif.py");

namespace {

struct GifContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  PixelCache cache;
  bool caching{false};
  bool success{true};
  uint32_t lastYieldMs{0};
};

void updateGrayPalette(const GIFDRAW* pDraw, GifContext& ctx) {
  const uint8_t* palette24 = pDraw->pPalette24;
  if (palette24 == nullptr || palette24 == ctx.activePalette24) return;

  ctx.activePalette24 = palette24;
  for (int i = 0; i < 256; ++i) {
    const uint8_t* pixel = &palette24[i * 3];
    ctx.grayPalette[i] = static_cast<uint8_t>((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
  }
}

void renderCanvasRow(const uint8_t* grayRow, const int srcY, GifContext& ctx) {
  // Same row mapping as the PNG decoder: downscaling may map several source rows to one output
  // row; upscaling repeats a source row over every output row in its range.
  int firstDstY = (srcY * ctx.dstHeight) / ctx.srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx.dstHeight > ctx.srcHeight) endDstY = ((srcY + 1) * ctx.dstHeight) / ctx.srcHeight;

  if (firstDstY <= ctx.lastDstY) firstDstY = ctx.lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx.dstHeight) return;
  if (endDstY > ctx.dstHeight) endDstY = ctx.dstHeight;

  DirectPixelWriter pw;
  pw.init(*ctx.renderer);

  for (int dstY = firstDstY; dstY < endDstY; ++dstY) {
    ctx.lastDstY = dstY;
    const int outY = ctx.config->y + dstY;
    // DirectPixelWriter clips rows but not columns; keep both axes in range (see PNG decoder).
    if (outY < 0 || outY >= ctx.screenHeight) continue;

    pw.beginRow(outY);

    // A cache flush failure stops caching for the rest of the decode; finalize() drops the file.
    bool caching = ctx.caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx.cache.advanceTo(dstY)) {
        caching = false;
        ctx.caching = false;
      } else {
        cw.init(ctx.cache.buffer, ctx.cache.bytesPerRow, ctx.cache.bandRows, ctx.cache.originX);
        cw.beginRow(outY, ctx.config->y + ctx.cache.bandStart);
      }
    }

    int srcX = 0;
    int error = 0;
    const int outXBase = ctx.config->x;
    const bool useDithering = ctx.config->useDithering;

    for (int dstX = 0; dstX < ctx.dstWidth; ++dstX) {
      const int outX = outXBase + dstX;
      if (outX >= 0 && outX < ctx.screenWidth) {
        const uint8_t gray = grayRow[srcX];
        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          ditheredGray = gray / 85;
          if (ditheredGray > 3) ditheredGray = 3;
        }
        pw.writePixel(outX, ditheredGray);
        if (caching) cw.writePixel(outX, ditheredGray);
      }

      // Bresenham-style stepping, no per-pixel division.
      error += ctx.srcWidth;
      while (error >= ctx.dstWidth) {
        error -= ctx.dstWidth;
        if (srcX + 1 < ctx.srcWidth) srcX++;
      }
    }
  }
}

// Rows a frame does not cover (a frame smaller than the canvas) are drawn as the white page.
void processGapRowsUpTo(const int canvasY, GifContext& ctx) {
  while (ctx.success && ctx.nextExpectedSrcY < canvasY && ctx.nextExpectedSrcY < ctx.srcHeight) {
    renderCanvasRow(ctx.whiteRow.get(), ctx.nextExpectedSrcY, ctx);
    ctx.nextExpectedSrcY++;
  }
}

void* gifOpenWithHandle(const char* filename, int32_t* size) {
  auto file = makeUniqueNoThrow<FsFile>();
  if (!file) {
    LOG_ERR("GIF", "OOM: file handle for %s", filename);
    return nullptr;
  }
  if (!Storage.openFileForRead("GIF", std::string(filename), *file)) return nullptr;
  *size = static_cast<int32_t>(file->size());
  return file.release();
}

void gifCloseWithHandle(void* handle) {
  auto* file = reinterpret_cast<FsFile*>(handle);
  if (!file) return;
  file->close();
  delete file;
}

int32_t gifReadWithHandle(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* file = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!file) return 0;
  const int32_t bytesRead = file->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t gifSeekWithHandle(GIFFILE* pFile, int32_t position) {
  auto* file = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!file) return -1;
  if (position < 0) position = 0;
  if (position >= pFile->iSize) position = pFile->iSize - 1;
  if (!file->seek(position)) return -1;
  pFile->iPos = position;
  return position;
}

void gifDrawCallback(GIFDRAW* pDraw) {
  auto* ctx = reinterpret_cast<GifContext*>(pDraw->pUser);
  if (!ctx || !ctx->success || !ctx->grayRow || !ctx->whiteRow) return;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  const int canvasY = pDraw->iY + pDraw->y;
  if (canvasY < 0 || canvasY >= ctx->srcHeight) {
    ctx->success = false;
    return;
  }

  processGapRowsUpTo(canvasY, *ctx);
  if (!ctx->success) return;

  memcpy(ctx->grayRow.get(), ctx->whiteRow.get(), static_cast<size_t>(ctx->srcWidth));
  updateGrayPalette(pDraw, *ctx);

  const uint8_t* pixels = pDraw->pPixels;
  const int startX = pDraw->iX;
  const int endX = startX + pDraw->iWidth;
  const bool hasTransparency = pDraw->ucHasTransparency != 0;
  const uint8_t transparent = pDraw->ucTransparent;

  // Transparent pixels show the white page.
  for (int srcX = startX, i = 0; srcX < endX && srcX < ctx->srcWidth; ++srcX, ++i) {
    if (srcX < 0) continue;
    const uint8_t index = pixels[i];
    if (hasTransparency && index == transparent) continue;
    ctx->grayRow[srcX] = ctx->grayPalette[index];
  }

  renderCanvasRow(ctx->grayRow.get(), canvasY, *ctx);
  ctx->nextExpectedSrcY = canvasY + 1;
}

bool readGifInfoFromPath(const std::string& imagePath, GifBasicInfo& info) {
  FsFile file;
  if (!Storage.openFileForRead("GIF", imagePath, file)) return false;
  const bool ok = GifCommon::readBasicInfo(file, info);
  file.close();
  return ok;
}

// The decoder is one block of sizeof(AnimatedGIF) (~26.7 KB on the C3 at MAX_WIDTH 3072: 24.1 KB
// of LZW tables and palettes plus the scanline). Gated like the PNG (~44 KB) and JPEG decoders:
// enough free heap for it plus headroom, and a free block that can hold it whole. When the gate
// fails the image takes the existing undecodable-image path (ImageBlock's placeholder) and is
// retried on the next render; once decoded, the pixel cache serves every later render, so the
// decoder is needed once per image.
constexpr size_t GIF_DECODER_BYTES = sizeof(AnimatedGIF);
constexpr size_t MIN_FREE_HEAP_FOR_GIF = GIF_DECODER_BYTES + 16 * 1024;
constexpr size_t MIN_MAX_ALLOC_FOR_GIF = GIF_DECODER_BYTES + 1024;

}  // namespace

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  GifBasicInfo info;
  if (!readGifInfoFromPath(imagePath, info)) {
    LOG_ERR("GIF", "Failed to read GIF dimensions: %s", imagePath.c_str());
    return false;
  }
  return validateAndStoreDimensions(info.canvasWidth, info.canvasHeight, out, "GIF");
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("GIF", "Decoding GIF: %s", imagePath.c_str());

  GifBasicInfo info;
  if (!readGifInfoFromPath(imagePath, info)) {
    LOG_ERR("GIF", "Failed to parse GIF header: %s", imagePath.c_str());
    return false;
  }
  if (info.interlaced) {
    warnUnsupportedFeature("interlaced GIF", imagePath);
    return false;
  }
  if (info.canvasWidth > MAX_WIDTH) {
    LOG_ERR("GIF", "GIF canvas %ux%u wider than the decoder's %d px: %s", info.canvasWidth, info.canvasHeight,
            MAX_WIDTH, imagePath.c_str());
    return false;
  }
  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(info.canvasWidth, info.canvasHeight, sourceDimensions, "GIF")) return false;

  const size_t freeBefore = ESP.getFreeHeap();
  const size_t maxAllocBefore = ESP.getMaxAllocHeap();
  if (freeBefore < MIN_FREE_HEAP_FOR_GIF || maxAllocBefore < MIN_MAX_ALLOC_FOR_GIF) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, %u max block; need %u / %u)",
            static_cast<unsigned>(freeBefore), static_cast<unsigned>(maxAllocBefore),
            static_cast<unsigned>(MIN_FREE_HEAP_FOR_GIF), static_cast<unsigned>(MIN_MAX_ALLOC_FOR_GIF));
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder (%u bytes)", static_cast<unsigned>(GIF_DECODER_BYTES));
    return false;
  }

  // ~330 bytes (the 256-entry gray palette dominates): heap, not the render task's stack.
  auto ctxPtr = makeUniqueNoThrow<GifContext>();
  if (!ctxPtr) {
    LOG_ERR("GIF", "OOM: GIF context");
    return false;
  }
  GifContext& ctx = *ctxPtr;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
  } else {
    const float scaleX = static_cast<float>(config.maxWidth) / ctx.srcWidth;
    const float scaleY = static_cast<float>(config.maxHeight) / ctx.srcHeight;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;  // never upscale
    ctx.dstWidth = static_cast<int>(ctx.srcWidth * scale);
    ctx.dstHeight = static_cast<int>(ctx.srcHeight * scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) {
    LOG_ERR("GIF", "Degenerate output dimensions %dx%d for %s", ctx.dstWidth, ctx.dstHeight, imagePath.c_str());
    return false;
  }

  // Two canvas-width rows (<= MAX_WIDTH bytes each), freed with the context.
  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  if (!ctx.grayRow || !ctx.whiteRow) {
    LOG_ERR("GIF", "OOM: GIF row buffers (%d bytes each)", ctx.srcWidth);
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  gif->begin(GIF_PALETTE_RGB888);
  gif->setDrawType(GIF_DRAW_RAW);
  const int rcOpen = gif->open(imagePath.c_str(), gifOpenWithHandle, gifCloseWithHandle, gifReadWithHandle,
                               gifSeekWithHandle, gifDrawCallback);
  if (rcOpen != 1) {
    LOG_ERR("GIF", "AnimatedGIF open failed: err=%d file=%s", gif->getLastError(), imagePath.c_str());
    return false;
  }
  const ScopedCleanup cleanup{[&gif]() { gif->close(); }};

  // Stream the pixel cache to disk: one output row per callback, so the band needs one row.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching && !ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
    LOG_ERR("GIF", "Failed to start cache stream, continuing without caching");
    ctx.caching = false;
  }

  // The working set is complete here: report it for heap audits (real numbers on device).
  LOG_DBG("GIF", "GIF %dx%d -> %dx%d, decoder %u B, working set %u B (free %u -> %u)", ctx.srcWidth, ctx.srcHeight,
          ctx.dstWidth, ctx.dstHeight, static_cast<unsigned>(GIF_DECODER_BYTES),
          static_cast<unsigned>(freeBefore - ESP.getFreeHeap()), static_cast<unsigned>(freeBefore),
          static_cast<unsigned>(ESP.getFreeHeap()));

  int delayMs = 0;
  const unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  const int frameState = gif->playFrame(false, &delayMs, &ctx);
  if (!ctx.success || (gif->getLastError() != GIF_SUCCESS && gif->getLastError() != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed (state=%d, err=%d)", frameState, gif->getLastError());
    // Drop the partial cache: a truncated .pxc would be rejected forever without a re-decode.
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  processGapRowsUpTo(ctx.srcHeight, ctx);
  if (!ctx.success) {
    LOG_ERR("GIF", "GIF post-process failed: %s", imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  if (frameState > 0) warnUnsupportedFeature("animation (first frame only)", imagePath);
  LOG_DBG("GIF", "GIF decoding complete - render time: %lu ms", millis() - decodeStart);

  if (ctx.caching) ctx.cache.finalize();
  return true;
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasGifExtension(extension);
}
