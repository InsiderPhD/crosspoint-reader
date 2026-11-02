#include "GifToBmpConverter.h"

#include <Arduino.h>
#include <GifCommon.h>
#include <GifImageLayout.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "BitmapHelpers.h"
#include "BufferedPrint.h"
#include "CoverToneCurve.h"

// Last: AnimatedGIF.h defines a MAX_WIDTH macro, which must not reach other headers.
#include <AnimatedGIF.h>

// Adapted from CrossPoint #2299 (lib/GifToBmpConverter). Differences: our BMP sinks go through
// BufferedPrint, and a luminance pre-pass feeds the shared cover tone curve exactly as the JPEG
// and PNG cover converters do, so a dark GIF cover is lifted the same way.

static_assert(MAX_WIDTH == CROSSPOINT_GIF_MAX_WIDTH, "AnimatedGIF not patched: run scripts/patch_animatedgif.py");

namespace {

constexpr uint16_t MAX_CANVAS_HEIGHT = 3072;

inline bool writeByte(Print& out, const uint8_t value) { return out.write(value) == 1; }

inline bool write16(Print& out, const uint16_t value) {
  return writeByte(out, static_cast<uint8_t>(value & 0xFF)) &&
         writeByte(out, static_cast<uint8_t>((value >> 8) & 0xFF));
}

inline bool write32(Print& out, const uint32_t value) {
  return writeByte(out, static_cast<uint8_t>(value & 0xFF)) &&
         writeByte(out, static_cast<uint8_t>((value >> 8) & 0xFF)) &&
         writeByte(out, static_cast<uint8_t>((value >> 16) & 0xFF)) &&
         writeByte(out, static_cast<uint8_t>((value >> 24) & 0xFF));
}

inline bool write32Signed(Print& out, const int32_t value) { return write32(out, static_cast<uint32_t>(value)); }

bool writeBmpHeader(Print& bmpOut, const int width, const int height, const bool oneBit) {
  const int bitsPerPixel = oneBit ? 1 : 2;
  const int colors = 1 << bitsPerPixel;
  const int bytesPerRow = (width * bitsPerPixel + 31) / 32 * 4;
  const int imageSize = bytesPerRow * height;
  const uint32_t headerSize = 54 + colors * 4;
  const uint32_t fileSize = headerSize + imageSize;

  if (!writeByte(bmpOut, 'B') || !writeByte(bmpOut, 'M') || !write32(bmpOut, fileSize) || !write32(bmpOut, 0) ||
      !write32(bmpOut, headerSize) || !write32(bmpOut, 40) || !write32Signed(bmpOut, width) ||
      !write32Signed(bmpOut, -height) || !write16(bmpOut, 1) || !write16(bmpOut, bitsPerPixel) || !write32(bmpOut, 0) ||
      !write32(bmpOut, imageSize) || !write32(bmpOut, 2835) || !write32(bmpOut, 2835) || !write32(bmpOut, colors) ||
      !write32(bmpOut, colors)) {
    return false;
  }

  for (int i = 0; i < colors; ++i) {
    const auto gray = static_cast<uint8_t>(i * (oneBit ? 255 : 85));
    if (!writeByte(bmpOut, gray) || !writeByte(bmpOut, gray) || !writeByte(bmpOut, gray) || !writeByte(bmpOut, 0))
      return false;
  }
  return true;
}

struct GifBmpContext {
  Print* bmpOut{nullptr};
  int srcWidth{0};
  int srcHeight{0};
  GifCommon::ImageLayout layout;
  int dstWidth{0};
  int dstHeight{0};
  bool oneBit{false};
  bool needsScaling{false};
  // Pre-pass: rows only feed the luminance histogram, nothing is written.
  LumaHistogram* histogram{nullptr};
  const uint8_t* toneLut{nullptr};
  uint32_t scaleX_fp{1u << 16};
  uint32_t scaleY_fp{1u << 16};
  int bytesPerRow{0};
  int currentOutY{0};
  uint32_t nextOutYSrcStart{0};
  int nextExpectedSrcY{0};
  const uint8_t* activePalette24{nullptr};
  uint8_t grayPalette[256] = {0};
  std::unique_ptr<uint8_t[]> grayRow;
  std::unique_ptr<uint8_t[]> scaledGrayRow;
  std::unique_ptr<uint8_t[]> whiteRow;
  std::unique_ptr<uint8_t[]> rowBuffer;
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;
  bool success{true};
  uint32_t lastYieldMs{0};
};

// The converters run one at a time (see sharedCoverToneCurve), so one file slot serves.
FsFile* s_gifFile = nullptr;

void* bmpGifOpen(const char* /*filename*/, int32_t* size) {
  if (!s_gifFile || !*s_gifFile) return nullptr;
  s_gifFile->seek(0);
  *size = static_cast<int32_t>(s_gifFile->size());
  return s_gifFile;
}

void bmpGifClose(void* /*handle*/) {}

int32_t bmpGifRead(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  if (!s_gifFile || !*s_gifFile) return 0;
  const int32_t bytesRead = s_gifFile->read(pBuf, len);
  if (bytesRead < 0) return 0;
  pFile->iPos += bytesRead;
  return bytesRead;
}

int32_t bmpGifSeek(GIFFILE* pFile, int32_t position) {
  if (!s_gifFile || !*s_gifFile) return -1;
  if (position < 0) position = 0;
  if (position >= pFile->iSize) position = pFile->iSize - 1;
  if (!s_gifFile->seek(position)) return -1;
  pFile->iPos = position;
  return position;
}

void updateGrayPalette(const GIFDRAW* pDraw, GifBmpContext& ctx) {
  const uint8_t* palette24 = pDraw->pPalette24;
  if (palette24 == nullptr || palette24 == ctx.activePalette24) return;

  ctx.activePalette24 = palette24;
  for (int i = 0; i < 256; ++i) {
    const uint8_t* pixel = &palette24[i * 3];
    ctx.grayPalette[i] = static_cast<uint8_t>((pixel[0] * 77 + pixel[1] * 150 + pixel[2] * 29) >> 8);
  }
}

void emitOutputRow(const uint8_t* row, GifBmpContext& ctx) {
  memset(ctx.rowBuffer.get(), 0, static_cast<size_t>(ctx.bytesPerRow));

  if (ctx.oneBit) {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      const uint8_t bit = ctx.atkinson1BitDitherer->processPixel(ctx.toneLut[row[x]], x);
      ctx.rowBuffer[x / 8] |= static_cast<uint8_t>(bit << (7 - (x % 8)));
    }
    ctx.atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      const uint8_t gray = adjustPixel(ctx.toneLut[row[x]]);
      const uint8_t twoBit = ctx.atkinsonDitherer->processPixel(gray, x);
      ctx.rowBuffer[(x * 2) / 8] |= static_cast<uint8_t>(twoBit << (6 - ((x * 2) % 8)));
    }
    ctx.atkinsonDitherer->nextRow();
  }

  const size_t bytesPerRow = static_cast<size_t>(ctx.bytesPerRow);
  if (ctx.bmpOut->write(ctx.rowBuffer.get(), bytesPerRow) != bytesPerRow) {
    LOG_ERR("GIF", "Short BMP row write");
    ctx.success = false;
  }
}

void processCanvasRow(const uint8_t* grayRow, int srcY, GifBmpContext& ctx) {
  if (!ctx.success || srcY < ctx.layout.sourceY || srcY >= ctx.layout.sourceY + ctx.layout.sourceHeight) return;
  srcY -= ctx.layout.sourceY;
  grayRow += ctx.layout.sourceX;

  if (ctx.histogram) {
    ctx.histogram->addRow(grayRow, ctx.layout.sourceWidth);
    return;
  }

  if (!ctx.needsScaling) {
    emitOutputRow(grayRow, ctx);
    return;
  }

  for (int outX = 0; outX < ctx.dstWidth; ++outX) {
    const int srcXStart = (static_cast<uint32_t>(outX) * ctx.scaleX_fp) >> 16;
    const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx.scaleX_fp) >> 16;

    int sum = 0;
    int count = 0;
    for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx.layout.sourceWidth; ++srcX) {
      sum += grayRow[srcX];
      count++;
    }
    if (count == 0 && srcXStart < ctx.layout.sourceWidth) {
      sum = grayRow[srcXStart];
      count = 1;
    }

    ctx.rowAccum[outX] += sum;
    ctx.rowCount[outX] += count;
  }

  const uint32_t srcY_fp = static_cast<uint32_t>(srcY + 1) << 16;
  while (ctx.success && srcY_fp >= ctx.nextOutYSrcStart && ctx.currentOutY < ctx.dstHeight) {
    for (int x = 0; x < ctx.dstWidth; ++x) {
      ctx.scaledGrayRow[x] = static_cast<uint8_t>((ctx.rowCount[x] > 0) ? (ctx.rowAccum[x] / ctx.rowCount[x]) : 255);
    }
    emitOutputRow(ctx.scaledGrayRow.get(), ctx);
    ctx.currentOutY++;
    ctx.nextOutYSrcStart = static_cast<uint32_t>(ctx.currentOutY + 1) * ctx.scaleY_fp;
    if (srcY_fp >= ctx.nextOutYSrcStart) continue;
    memset(ctx.rowAccum.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
    memset(ctx.rowCount.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
  }
}

void processGapRowsUpTo(int canvasY, GifBmpContext& ctx) {
  while (ctx.success && ctx.nextExpectedSrcY < canvasY && ctx.nextExpectedSrcY < ctx.srcHeight) {
    processCanvasRow(ctx.whiteRow.get(), ctx.nextExpectedSrcY, ctx);
    ctx.nextExpectedSrcY++;
  }
}

void gifBmpDrawCallback(GIFDRAW* pDraw) {
  auto* ctx = reinterpret_cast<GifBmpContext*>(pDraw->pUser);
  if (!ctx || !ctx->success || !ctx->grayRow || !ctx->whiteRow) return;

  const uint32_t now = millis();
  if (now - ctx->lastYieldMs >= 250) {
    ctx->lastYieldMs = now;
    vTaskDelay(1);
  }

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

  for (int srcX = startX, i = 0; srcX < endX && srcX < ctx->srcWidth; ++srcX, ++i) {
    if (srcX < 0) continue;
    const uint8_t index = pixels[i];
    if (hasTransparency && index == transparent) continue;
    ctx->grayRow[srcX] = ctx->grayPalette[index];
  }

  processCanvasRow(ctx->grayRow.get(), canvasY, *ctx);
  ctx->nextExpectedSrcY = canvasY + 1;
}

// One first-frame decode through the draw callback. Rewinds the decoder first so it can run twice.
bool playFirstFrame(AnimatedGIF& gif, GifBmpContext& ctx, int& frameState) {
  gif.reset();
  ctx.success = true;
  ctx.nextExpectedSrcY = 0;
  ctx.activePalette24 = nullptr;
  int delayMs = 0;
  ctx.lastYieldMs = millis();
  frameState = gif.playFrame(false, &delayMs, &ctx);
  if (!ctx.success || (gif.getLastError() != GIF_SUCCESS && gif.getLastError() != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed (state=%d, err=%d)", frameState, gif.getLastError());
    return false;
  }
  processGapRowsUpTo(ctx.srcHeight, ctx);
  return ctx.success;
}

// The decoder (sizeof(AnimatedGIF), one block) plus the converter's own working set: row buffers,
// scaling accumulators, ditherer rows and the 2 KB BufferedPrint, all a few KB at cover sizes.
constexpr size_t GIF_DECODER_BYTES = sizeof(AnimatedGIF);
constexpr size_t MIN_FREE_HEAP = GIF_DECODER_BYTES + 32 * 1024;
constexpr size_t MIN_MAX_ALLOC = GIF_DECODER_BYTES + 1024;

}  // namespace

bool GifToBmpConverter::hasHeapForDecoder() {
  return ESP.getFreeHeap() >= MIN_FREE_HEAP && ESP.getMaxAllocHeap() >= MIN_MAX_ALLOC;
}

bool GifToBmpConverter::gifFileToBmpStreamInternal(FsFile& gifFile, Print& sink, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop) {
  LOG_DBG("GIF", "Converting GIF to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  if (!hasHeapForDecoder()) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, %u max block, need %u / %u)",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(MIN_FREE_HEAP), static_cast<unsigned>(MIN_MAX_ALLOC));
    return false;
  }

  GifBasicInfo info;
  if (!GifCommon::readBasicInfo(gifFile, info)) {
    LOG_ERR("GIF", "Invalid GIF header");
    return false;
  }
  if (info.interlaced) {
    LOG_ERR("GIF", "Interlaced GIFs are not supported for BMP conversion");
    return false;
  }
  if (info.canvasWidth == 0 || info.canvasHeight == 0 || info.canvasWidth > MAX_WIDTH ||
      info.canvasHeight > MAX_CANVAS_HEIGHT) {
    LOG_ERR("GIF", "Image too large or zero (%ux%u)", info.canvasWidth, info.canvasHeight);
    return false;
  }

  auto gif = makeUniqueNoThrow<AnimatedGIF>();
  if (!gif) {
    LOG_ERR("GIF", "OOM: AnimatedGIF decoder (%u bytes)", static_cast<unsigned>(GIF_DECODER_BYTES));
    return false;
  }

  auto ctxPtr = makeUniqueNoThrow<GifBmpContext>();
  if (!ctxPtr) {
    LOG_ERR("GIF", "OOM: GIF BMP context");
    return false;
  }
  GifBmpContext& ctx = *ctxPtr;
  BufferedPrint bmpOut(sink);
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = info.canvasWidth;
  ctx.srcHeight = info.canvasHeight;
  ctx.oneBit = oneBit;

  if (!GifCommon::calculateLayout(ctx.srcWidth, ctx.srcHeight, targetWidth, targetHeight, crop, ctx.layout)) {
    LOG_ERR("GIF", "Invalid GIF output dimensions: %dx%d", targetWidth, targetHeight);
    return false;
  }
  ctx.dstWidth = ctx.layout.width;
  ctx.dstHeight = ctx.layout.height;
  ctx.scaleX_fp = (static_cast<uint32_t>(ctx.layout.sourceWidth) << 16) / ctx.dstWidth;
  ctx.scaleY_fp = (static_cast<uint32_t>(ctx.layout.sourceHeight) << 16) / ctx.dstHeight;
  ctx.needsScaling = ctx.layout.sourceWidth != ctx.dstWidth || ctx.layout.sourceHeight != ctx.dstHeight;
  ctx.nextOutYSrcStart = ctx.scaleY_fp;
  const uint32_t bitsPerPixel = oneBit ? 1 : 2;
  const uint32_t rowBytes = ((static_cast<uint32_t>(ctx.dstWidth) * bitsPerPixel + 31) / 32) * 4;
  const uint32_t headerBytes = 54 + (1u << bitsPerPixel) * 4;
  if (static_cast<uint64_t>(rowBytes) * ctx.dstHeight + headerBytes > GifCommon::MAX_BMP_BYTES) {
    LOG_ERR("GIF", "GIF BMP exceeds output byte limit");
    return false;
  }
  ctx.bytesPerRow = static_cast<int>(rowBytes);

  // Row buffers: canvas width (<= 3072) twice, plus the output row. Heap, not stack: far over
  // the 256-byte local budget.
  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.whiteRow = makeUniqueNoThrow<uint8_t[]>(ctx.srcWidth);
  ctx.rowBuffer = makeUniqueNoThrow<uint8_t[]>(ctx.bytesPerRow);
  if (!ctx.grayRow || !ctx.whiteRow || !ctx.rowBuffer) {
    LOG_ERR("GIF", "OOM: GIF BMP row buffers");
    return false;
  }
  memset(ctx.whiteRow.get(), 255, static_cast<size_t>(ctx.srcWidth));

  s_gifFile = &gifFile;
  const ScopedCleanup fileCleanup{[]() { s_gifFile = nullptr; }};
  gif->begin(GIF_PALETTE_RGB888);
  gif->setDrawType(GIF_DRAW_RAW);
  const int rcOpen = gif->open("", bmpGifOpen, bmpGifClose, bmpGifRead, bmpGifSeek, gifBmpDrawCallback);
  if (rcOpen != 1) {
    LOG_ERR("GIF", "Failed to open GIF (err=%d)", gif->getLastError());
    return false;
  }
  const ScopedCleanup gifCleanup{[&gif]() { gif->close(); }};

  // Luminance pre-pass for the cover tone curve (CoverToneCurve.h). GIF has no reduced decode,
  // so this is a full first decode of frame one; covers convert once and are cached. A failed
  // pre-pass converts with the identity curve.
  int frameState = 0;
  LumaHistogram hist;
  ctx.histogram = &hist;
  if (!playFirstFrame(*gif, ctx, frameState)) {
    LOG_ERR("GIF", "Luminance pre-pass failed; converting without tone curve");
    hist.reset();
  }
  ctx.histogram = nullptr;
  CoverToneCurve& toneCurve = sharedCoverToneCurve();
  toneCurve.build(hist);
  ctx.toneLut = toneCurve.table();

  if (!writeBmpHeader(bmpOut, ctx.dstWidth, ctx.dstHeight, oneBit)) {
    LOG_ERR("GIF", "Failed to write BMP header");
    return false;
  }

  if (ctx.needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(ctx.dstWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(ctx.dstWidth);
    ctx.scaledGrayRow = makeUniqueNoThrow<uint8_t[]>(ctx.dstWidth);
    if (!ctx.rowAccum || !ctx.rowCount || !ctx.scaledGrayRow) {
      LOG_ERR("GIF", "OOM: GIF scaling accumulators");
      return false;
    }
    memset(ctx.rowAccum.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
    memset(ctx.rowCount.get(), 0, static_cast<size_t>(ctx.dstWidth) * sizeof(uint32_t));
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(ctx.dstWidth);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->isValid()) {
      LOG_ERR("GIF", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else {
    ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(ctx.dstWidth);
    if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->isValid()) {
      LOG_ERR("GIF", "OOM: AtkinsonDitherer");
      return false;
    }
  }

  if (!playFirstFrame(*gif, ctx, frameState)) {
    LOG_ERR("GIF", "GIF BMP conversion failed");
    return false;
  }
  if (!bmpOut.flushBuffer()) {
    LOG_ERR("GIF", "Failed to flush BMP output");
    return false;
  }
  if (frameState > 0) LOG_INF("GIF", "Animated GIF: using the first frame for the BMP");
  LOG_DBG("GIF", "Converted GIF to BMP (%dx%d)", ctx.dstWidth, ctx.dstHeight);
  return true;
}

bool GifToBmpConverter::gifFileToBmpStream(FsFile& gifFile, Print& bmpOut, const bool crop) {
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetWidth, targetHeight, false, crop);
}

bool GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(FsFile& gifFile, Print& bmpOut, const int targetMaxWidth,
                                                       const int targetMaxHeight) {
  return gifFileToBmpStreamInternal(gifFile, bmpOut, targetMaxWidth, targetMaxHeight, true, false);
}
