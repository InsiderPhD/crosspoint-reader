#include "SleepOverlay.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cmath>

namespace {

// PNGdec hands its file callbacks no user pointer at open time, so the file
// they read lives here for the length of one drawPng() call. It points at a
// local of that call, which avoids the heap-allocated FsFile the EPUB image
// path uses. Sleep entry is single-threaded on the loop task.
FsFile* activePngFile = nullptr;

void* pngOpen(const char* filename, int32_t* size) {
  if (!activePngFile || !Storage.openFileForRead("SLP", filename, *activePngFile)) {
    return nullptr;
  }
  *size = static_cast<int32_t>(activePngFile->size());
  return activePngFile;
}

void pngClose(void* handle) {
  auto* file = static_cast<FsFile*>(handle);
  if (file) file->close();
}

int32_t pngRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* file = static_cast<FsFile*>(pFile->fHandle);
  return file ? file->read(pBuf, len) : 0;
}

int32_t pngSeek(PNGFILE* pFile, int32_t pos) {
  auto* file = static_cast<FsFile*>(pFile->fHandle);
  return file ? file->seek(pos) : -1;
}

struct PngOverlayContext {
  const GfxRenderer* renderer;
  PNG* decoder;
  int screenWidth;
  int screenHeight;
  int srcWidth;
  int srcHeight;
  int dstWidth;
  int dstHeight;
  int dstX;
  int dstY;
  int lastDstRow;  // last destination row drawn, so a downscale skips repeats
};

// 4x4 ordered dither. Each entry is the gray level below which a pixel at that
// position prints black: 0 is always black, 255 always white.
constexpr uint8_t BAYER_THRESHOLD[4][4] = {
    {8, 136, 40, 168},
    {200, 72, 232, 104},
    {56, 184, 24, 152},
    {248, 120, 216, 88},
};

uint8_t luma(const uint8_t* rgb) { return static_cast<uint8_t>((rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8); }

// Sub-byte samples are packed most-significant sample first within each byte.
uint8_t packedSample(const uint8_t* pixels, const int x, const int bitsPerSample) {
  if (bitsPerSample >= 8) return pixels[x];
  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  return static_cast<uint8_t>((pixels[bitOffset >> 3] >> shift) & ((1U << bitsPerSample) - 1));
}

int bytesPerPixel(const int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    default:
      return 1;
  }
}

bool supportedBitDepth(const int pixelType, const int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

// PNGdec keeps the current and the previous scanline, each with a filter byte,
// in a fixed internal buffer of PNG_MAX_BUFFERED_PIXELS bytes and does not
// check that an image fits. Same bound as PngToFramebufferConverter.
int pngInternalBufferBytes(const int srcWidth, const int pixelType, const int bitsPerSample) {
  int pitch = srcWidth * bytesPerPixel(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = (srcWidth * bitsPerSample + 7) / 8;
  }
  return ((pitch + 1) * 2) + 32;
}

// One decoded scanline -> one framebuffer row. No intermediate row buffer: each
// source pixel is read where PNGdec left it and written with drawPixel.
int pngOverlayDraw(PNGDRAW* pDraw) {
  auto* ctx = static_cast<PngOverlayContext*>(pDraw->pUser);
  if (!ctx) return 0;

  // Integer row mapping (the C3 has no FPU). The image is never upscaled, so
  // several source rows can land on one destination row; draw the first only.
  const int dstRow = (pDraw->y * ctx->dstHeight) / ctx->srcHeight;
  if (dstRow <= ctx->lastDstRow || dstRow >= ctx->dstHeight) return 1;
  ctx->lastDstRow = dstRow;
  const int outY = ctx->dstY + dstRow;
  if (outY < 0 || outY >= ctx->screenHeight) return 1;

  const uint8_t* pixels = pDraw->pPixels;
  const int pixelType = pDraw->iPixelType;
  const int bitsPerSample = pDraw->iBpp;
  const bool hasAlpha = pDraw->iHasAlpha != 0;
  // tRNS colour key for the two pixel types without an alpha channel. PNGdec
  // parses the chunk during decode(), so it is only valid from here on.
  const uint32_t colorKey = (hasAlpha && ctx->decoder) ? ctx->decoder->getTransparentColor() : 0;
  const uint8_t* thresholds = BAYER_THRESHOLD[outY & 3];

  int srcX = 0;
  int error = 0;
  for (int col = 0; col < ctx->dstWidth; col++) {
    const int outX = ctx->dstX + col;
    if (outX >= 0 && outX < ctx->screenWidth) {
      uint8_t alpha = 255;
      uint8_t gray = 0;
      switch (pixelType) {
        case PNG_PIXEL_TRUECOLOR_ALPHA: {
          const uint8_t* p = &pixels[srcX * 4];
          alpha = p[3];
          gray = luma(p);
          break;
        }
        case PNG_PIXEL_GRAY_ALPHA:
          gray = pixels[srcX * 2];
          alpha = pixels[srcX * 2 + 1];
          break;
        case PNG_PIXEL_TRUECOLOR: {
          const uint8_t* p = &pixels[srcX * 3];
          gray = luma(p);
          if (hasAlpha && p[0] == ((colorKey >> 16) & 0xFF) && p[1] == ((colorKey >> 8) & 0xFF) &&
              p[2] == (colorKey & 0xFF)) {
            alpha = 0;
          }
          break;
        }
        case PNG_PIXEL_INDEXED: {
          const uint8_t index = packedSample(pixels, srcX, bitsPerSample);
          if (pDraw->pPalette) {
            gray = luma(&pDraw->pPalette[index * 3]);
            if (hasAlpha) alpha = pDraw->pPalette[768 + index];
          }
          break;
        }
        case PNG_PIXEL_GRAYSCALE:
        default: {
          const uint8_t sample = packedSample(pixels, srcX, bitsPerSample);
          if (hasAlpha && sample == (colorKey & 0xFF)) alpha = 0;
          gray = (bitsPerSample >= 8) ? sample : static_cast<uint8_t>((sample * 255U) / ((1U << bitsPerSample) - 1));
          break;
        }
      }

      // Below 50% alpha the page underneath stays as it is.
      if (alpha >= 128) {
        ctx->renderer->drawPixel(outX, outY, gray < thresholds[outX & 3]);
      }
    }

    // Bresenham-style stepping through the source row (1:1 when unscaled).
    error += ctx->srcWidth;
    while (error >= ctx->dstWidth) {
      error -= ctx->dstWidth;
      srcX++;
    }
  }
  return 1;
}

}  // namespace

SleepOverlay::Result SleepOverlay::drawPng(GfxRenderer& renderer, const char* path) {
  if (!Storage.exists(path)) {
    return Result::NotFound;
  }

  // The decoder object is the one allocation here: sizeof(PNG), 45-60KB on the
  // device, nearly all of it the 32KB inflate window a PNG cannot be read
  // without plus PNGdec's two-scanline buffer (PNG_MAX_BUFFERED_PIXELS). The
  // exact figure is in the log line below. It is one contiguous block, taken after the
  // outgoing activity has been torn down and Bluetooth switched off, and freed
  // before this function returns. When the heap has no such block the overlay
  // is skipped; the caller still has the page to show.
  auto png = makeUniqueNoThrow<PNG>();
  if (!png) {
    LOG_ERR("SLP", "No memory for PNG overlay decoder (%u bytes, free %u, largest %u): %s",
            static_cast<unsigned>(sizeof(PNG)), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()), path);
    return Result::Failed;
  }

  FsFile file;
  activePngFile = &file;
  const auto finish = [&](const Result result) {
    png->close();
    if (file) file.close();
    activePngFile = nullptr;
    return result;
  };

  int rc = png->open(path, pngOpen, pngClose, pngRead, pngSeek, pngOverlayDraw);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("SLP", "PNG overlay open failed (%d): %s", rc, path);
    return finish(Result::Failed);
  }

  const int srcWidth = png->getWidth();
  const int srcHeight = png->getHeight();
  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  if (srcWidth <= 0 || srcHeight <= 0 || !supportedBitDepth(pixelType, bitsPerSample) ||
      pngInternalBufferBytes(srcWidth, pixelType, bitsPerSample) > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR("SLP", "Unsupported PNG overlay (%dx%d type %d, %d bpp): %s", srcWidth, srcHeight, pixelType, bitsPerSample,
            path);
    return finish(Result::Failed);
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int dstWidth = srcWidth;
  int dstHeight = srcHeight;
  if (srcWidth > screenWidth || srcHeight > screenHeight) {
    // Fit inside the screen, keeping the aspect ratio. Cross-multiplied so the
    // comparison and both divisions stay in integers.
    if (static_cast<int64_t>(srcWidth) * screenHeight > static_cast<int64_t>(srcHeight) * screenWidth) {
      dstWidth = screenWidth;
      dstHeight = static_cast<int>(static_cast<int64_t>(srcHeight) * screenWidth / srcWidth);
    } else {
      dstHeight = screenHeight;
      dstWidth = static_cast<int>(static_cast<int64_t>(srcWidth) * screenHeight / srcHeight);
    }
    if (dstWidth < 1) dstWidth = 1;
    if (dstHeight < 1) dstHeight = 1;
  }

  PngOverlayContext ctx{};
  ctx.renderer = &renderer;
  ctx.decoder = png.get();
  ctx.screenWidth = screenWidth;
  ctx.screenHeight = screenHeight;
  ctx.srcWidth = srcWidth;
  ctx.srcHeight = srcHeight;
  ctx.dstWidth = dstWidth;
  ctx.dstHeight = dstHeight;
  ctx.dstX = (screenWidth - dstWidth) / 2;
  ctx.dstY = (screenHeight - dstHeight) / 2;
  ctx.lastDstRow = -1;

  LOG_INF("SLP", "PNG overlay %dx%d -> %dx%d: %s", srcWidth, srcHeight, dstWidth, dstHeight, path);
  rc = png->decode(&ctx, 0);
  if (rc != PNG_SUCCESS) {
    LOG_ERR("SLP", "PNG overlay decode failed (%d): %s", rc, path);
    return finish(Result::Failed);
  }
  return finish(Result::Drawn);
}

SleepOverlay::Result SleepOverlay::drawBmp(GfxRenderer& renderer, const char* path) {
  if (!Storage.exists(path)) {
    return Result::NotFound;
  }
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    return Result::Failed;
  }
  // Dithering stays off: error diffusion would speckle the nominally white,
  // i.e. transparent, areas with black pixels over the page.
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid BMP overlay: %s", path);
    file.close();
    return Result::Failed;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int x;
  int y;
  if (bitmap.getWidth() > screenWidth || bitmap.getHeight() > screenHeight) {
    // drawBitmap scales to fit; centre the scaled result on the other axis.
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(screenWidth) / static_cast<float>(screenHeight);
    if (ratio > screenRatio) {
      x = 0;
      y = static_cast<int>(
          std::round((static_cast<float>(screenHeight) - static_cast<float>(screenWidth) / ratio) / 2));
    } else {
      x = static_cast<int>(
          std::round((static_cast<float>(screenWidth) - static_cast<float>(screenHeight) * ratio) / 2));
      y = 0;
    }
  } else {
    x = (screenWidth - bitmap.getWidth()) / 2;
    y = (screenHeight - bitmap.getHeight()) / 2;
  }

  // In BW mode drawBitmap only ever sets black pixels and leaves white ones
  // untouched, which is exactly the transparency wanted here. Its row buffers
  // are a few hundred bytes, allocated and freed inside the call.
  LOG_INF("SLP", "BMP overlay %dx%d: %s", bitmap.getWidth(), bitmap.getHeight(), path);
  renderer.drawBitmap(bitmap, x, y, screenWidth, screenHeight, 0, 0);
  file.close();
  return Result::Drawn;
}
