#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline bool write16(Print& out, const uint16_t value) {
  return (out.write(value & 0xFF) == 1) && (out.write((value >> 8) & 0xFF) == 1);
}

inline bool write32(Print& out, const uint32_t value) {
  return (out.write(value & 0xFF) == 1) && (out.write((value >> 8) & 0xFF) == 1) &&
         (out.write((value >> 16) & 0xFF) == 1) && (out.write((value >> 24) & 0xFF) == 1);
}

inline bool write32Signed(Print& out, const int32_t value) {
  return (out.write(value & 0xFF) == 1) && (out.write((value >> 8) & 0xFF) == 1) &&
         (out.write((value >> 16) & 0xFF) == 1) && (out.write((value >> 24) & 0xFF) == 1);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static bool writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  if (bmpOut.write('B') != 1 || bmpOut.write('M') != 1) {
    LOG_ERR("JPG", "Failed to write BMP signature");
    return false;
  }

  if (!write32(bmpOut, fileSize) || !write32(bmpOut, 0) || !write32(bmpOut, 70)) {
    LOG_ERR("JPG", "Failed to write BMP file header");
    return false;
  }

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  if (!write32(bmpOut, 40) || !write32Signed(bmpOut, width) || !write32Signed(bmpOut, -height) || !write16(bmpOut, 1) ||
      !write16(bmpOut, 2) || !write32(bmpOut, 0) || !write32(bmpOut, imageSize) || !write32(bmpOut, 2835) ||
      !write32(bmpOut, 2835) || !write32(bmpOut, 4) || !write32(bmpOut, 4)) {
    LOG_ERR("JPG", "Failed to write BMP DIB header");
    return false;
  }

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    if (bmpOut.write(i) != 1) {
      LOG_ERR("JPG", "Failed to write BMP palette");
      return false;
    }
  }

  LOG_DBG("JPG", "Successfully wrote BMP header and palette (%d bytes)", 70);
  return true;
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static FsFile* s_jpegFile = nullptr;

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  uint8_t* mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  uint32_t* rowAccum;
  uint32_t* rowCount;

  uint8_t* bmpRow;

  AtkinsonDitherer* atkinsonDitherer;
  FloydSteinbergDitherer* fsDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;

  int rowsWritten;
  int callbackCount;
  bool error;
};

static int scaledDecodeDimension(const int value, const int scaleShift) {
  if (scaleShift <= 0) return value;
  return (value + (1 << scaleShift) - 1) >> scaleShift;
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  int bytesWritten = ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  if (bytesWritten != ctx->bytesPerRow) {
    LOG_ERR("JPG", "Row write failed: expected %d bytes, wrote %d bytes, freeHeap=%d", ctx->bytesPerRow, bytesWritten,
            ESP.getFreeHeap());
    ctx->error = true;
  } else {
    ctx->rowsWritten++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  int bytesWritten = ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  if (bytesWritten != ctx->bytesPerRow) {
    LOG_ERR("JPG", "Row write failed: expected %d bytes, wrote %d bytes, freeHeap=%d", ctx->bytesPerRow, bytesWritten,
            ESP.getFreeHeap());
    ctx->error = true;
  } else {
    ctx->rowsWritten++;
  }
  ctx->currentOutY++;
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) {
    LOG_ERR("JPG", "Draw callback: ctx=%p, error=%d", ctx, ctx ? ctx->error : -1);
    return 0;
  }

  if (ctx->callbackCount++ < 3) {
    LOG_DBG("JPG", "Draw callback %d: %dx%d block at (%d,%d)", ctx->callbackCount, pDraw->iWidth, pDraw->iHeight,
            pDraw->x, pDraw->y);
  }

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf + (y - blockY) * ctx->srcWidth;

    if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum, 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount, 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop) {
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  s_jpegFile = &jpegFile;

  JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    delete jpeg;
    return false;
  }

  int srcWidth = jpeg->getWidth();
  int srcHeight = jpeg->getHeight();
  const bool progressive = jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE;

  LOG_DBG("JPG", "JPEG dimensions: %dx%d%s", srcWidth, srcHeight, progressive ? " (progressive)" : "");

  // Sanity bound to reject malformed JPEGs / overflow-prone headers up front.
  // The real memory-budget bound is applied after native scaled-decode below, since
  // 1/8-scale decode can bring an iPad-class 6000x9000 cover down to 750x1125 well
  // within budget. BookFusion covers in particular routinely exceed the old
  // 2048x3072 raw bound, which would otherwise reject them before scaling rescues them.
  constexpr int RAW_SANITY_WIDTH = 16384;
  constexpr int RAW_SANITY_HEIGHT = 24576;
  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > RAW_SANITY_WIDTH || srcHeight > RAW_SANITY_HEIGHT) {
    LOG_DBG("JPG", "Image invalid or insanely large (%dx%d)", srcWidth, srcHeight);
    jpeg->close();
    delete jpeg;
    return false;
  }

  // Native scaled-decode: pick the largest 1/N (N=2,4,8) that still produces an image
  // at least as wide and tall as the target. JPEGDEC's _HALF/_QUARTER/_EIGHTH flags
  // skip DCT coefficients during decode — both decode time and the MCU buffer scale
  // down with N². Existing area-averaging downstream polishes to exact target dims.
  int scaleFlag = 0;
  int scaleShift = 0;
  if (progressive) {
    // JPEGDEC only decodes the first DC scan of progressive JPEGs and forces
    // 1/8 output internally. Mirror that here so callback row assembly waits
    // for the width JPEGDEC actually emits, not the full source width.
    scaleFlag = JPEG_SCALE_EIGHTH;
    scaleShift = 3;
  } else if (targetWidth > 0 && targetHeight > 0) {
    if ((srcWidth >> 3) >= targetWidth && (srcHeight >> 3) >= targetHeight) {
      scaleFlag = JPEG_SCALE_EIGHTH;
      scaleShift = 3;
    } else if ((srcWidth >> 2) >= targetWidth && (srcHeight >> 2) >= targetHeight) {
      scaleFlag = JPEG_SCALE_QUARTER;
      scaleShift = 2;
    } else if ((srcWidth >> 1) >= targetWidth && (srcHeight >> 1) >= targetHeight) {
      scaleFlag = JPEG_SCALE_HALF;
      scaleShift = 1;
    }
  }
  if (scaleShift > 0) {
    const int scaledWidth = scaledDecodeDimension(srcWidth, scaleShift);
    const int scaledHeight = scaledDecodeDimension(srcHeight, scaleShift);
    LOG_DBG("JPG", "Native scaled decode 1/%d: %dx%d -> %dx%d effective", 1 << scaleShift, srcWidth, srcHeight,
            scaledWidth, scaledHeight);
    srcWidth = scaledWidth;
    srcHeight = scaledHeight;
  }

  // Memory-budget bound on the *post-scale* dimensions — these drive the MCU
  // buffer alloc (MAX_MCU_HEIGHT * srcWidth) and the per-row scaling buffers below.
  constexpr int MAX_EFFECTIVE_WIDTH = 2048;
  constexpr int MAX_EFFECTIVE_HEIGHT = 3072;
  if (srcWidth > MAX_EFFECTIVE_WIDTH || srcHeight > MAX_EFFECTIVE_HEIGHT) {
    LOG_DBG("JPG", "Image still too large after 1/%d scale (%dx%d effective, max %dx%d)", 1 << scaleShift, srcWidth,
            srcHeight, MAX_EFFECTIVE_WIDTH, MAX_EFFECTIVE_HEIGHT);
    jpeg->close();
    delete jpeg;
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = srcWidth;
  int outHeight = srcHeight;
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (static_cast<uint32_t>(srcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(srcHeight) << 16) / outHeight;
    needsScaling = true;

    LOG_DBG("JPG", "Scaling %dx%d -> %dx%d (target %dx%d)", srcWidth, srcHeight, outWidth, outHeight, targetWidth,
            targetHeight);
  }

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
    const int expectedImageSize = bytesPerRow * outHeight;
    const int expectedFileSize = 70 + expectedImageSize;
    LOG_DBG("JPG", "Writing 2-bit BMP: %dx%d, bytesPerRow=%d, imageSize=%d, fileSize=%d, freeHeap=%d", outWidth,
            outHeight, bytesPerRow, expectedImageSize, expectedFileSize, ESP.getFreeHeap());

    if (!writeBmpHeader2bit(bmpOut, outWidth, outHeight)) {
      LOG_ERR("JPG", "Failed to write BMP header - buffer may be full");
      return false;
    }
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = srcWidth;
  ctx.srcHeight = srcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.rowsWritten = 0;
  ctx.callbackCount = 0;
  ctx.error = false;

  // RAII guard: frees all heap resources on any return path
  struct Cleanup {
    BmpConvertCtx& ctx;
    JPEGDEC* jpeg;
    ~Cleanup() {
      delete[] ctx.rowAccum;
      delete[] ctx.rowCount;
      delete ctx.atkinsonDitherer;
      delete ctx.fsDitherer;
      delete ctx.atkinson1BitDitherer;
      free(ctx.mcuBuf);
      free(ctx.bmpRow);
      jpeg->close();
      delete jpeg;
    }
  } cleanup{ctx, jpeg};

  // MCU row buffer: MAX_MCU_HEIGHT rows × srcWidth columns of grayscale
  ctx.mcuBuf = static_cast<uint8_t*>(malloc(MAX_MCU_HEIGHT * srcWidth));
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "Failed to allocate MCU buffer (%d bytes)", MAX_MCU_HEIGHT * srcWidth);
    return false;
  }
  memset(ctx.mcuBuf, 0, MAX_MCU_HEIGHT * srcWidth);

  ctx.bmpRow = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "Failed to allocate BMP row buffer");
    return false;
  }

  if (needsScaling) {
    ctx.rowAccum = new (std::nothrow) uint32_t[outWidth]();
    ctx.rowCount = new (std::nothrow) uint32_t[outWidth]();
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "Failed to allocate scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = new (std::nothrow) FloydSteinbergDitherer(outWidth);
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  LOG_DBG("JPG", "Starting JPEG decode for %dx%d -> %dx%d", srcWidth, srcHeight, outWidth, outHeight);

  rc = jpeg->decode(0, 0, scaleFlag);

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d, ctx.error=%d)", rc, jpeg->getLastError(), ctx.error);
    return false;
  }

  if (ctx.rowsWritten != ctx.outHeight) {
    LOG_ERR("JPG", "JPEG decode incomplete: wrote %d/%d BMP rows", ctx.rowsWritten, ctx.outHeight);
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP: %d rows written", ctx.rowsWritten);
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true);
}
