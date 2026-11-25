#include "SleepGray16.h"

#if CROSSPOINT_GRAY16_IMAGE

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "SleepCover.h"
#include "sleeptheme/SleepThemeRenderer.h"

namespace SleepGray16 {
namespace {

// L* of each level (0 = white .. 15 = darkest) as a clean 16-level draw shows
// it, from the display driver (HalDisplay::gray16LevelLstar, L* x10). A sleep
// screen always draws clean, so these are the tones it shows. Filled by
// present() before any image is dithered.
float levelLstar[16] = {};

void* psram(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

bool shown = false;

struct Levels {
  uint8_t* data;
  uint16_t w, h;  // panel-native
  void set(int px, int py, uint8_t level) const {
    if (px < 0 || py < 0 || px >= w || py >= h) return;
    uint8_t& b = data[static_cast<size_t>(py) * (w / 2) + px / 2];
    b = (px & 1) ? static_cast<uint8_t>((b & 0xF0) | level) : static_cast<uint8_t>((b & 0x0F) | level << 4);
  }
};

// sRGB byte -> L* (0..100), tabulated once: powf/cbrtf per pixel cost ~2.5 s
// on a full-screen cover. 1 KB of static RAM (DRAM .bss; Picco only).
float lumToLstar(uint8_t v) {
  static float table[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; ++i) {
      const float c = i / 255.0f;
      const float y = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
      const float f = y > 0.008856f ? cbrtf(y) : y / (3.0f * (6.0f / 29.0f) * (6.0f / 29.0f)) + 4.0f / 29.0f;
      table[i] = 116.0f * f - 16.0f;
    }
    ready = true;
  }
  return table[v];
}

// One grey image into `levels`, placed as GfxRenderer::drawBitmap places it
// (same crop, downscale-only fit, row order and pixel mapping), tones mapped
// onto the panel's white..black span and Floyd-Steinberg dithered to the
// nearest level in L*.
void drawImage(const GfxRenderer& renderer, const Levels& out, const std::string& path,
               const SleepThemeRenderer::GreyImage& g) {
  FsFile file;
  if (!Storage.openFileForRead("SG16", path, file)) return;
  Bitmap bitmap(file, false);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;
  const int bw = bitmap.getWidth(), bh = bitmap.getHeight();
  const int cropPixX = static_cast<int>(std::floor(bw * g.cropX / 2.0f));
  const int cropPixY = static_cast<int>(std::floor(bh * g.cropY / 2.0f));
  const float croppedW = (1.0f - g.cropX) * bw, croppedH = (1.0f - g.cropY) * bh;
  float fit = 1.0f;
  bool bounded = false;
  if (g.width > 0 && croppedW > 0.0f) {
    fit = g.width / croppedW;
    bounded = true;
  }
  if (g.height > 0 && croppedH > 0.0f) {
    const float hs = g.height / croppedH;
    fit = bounded ? std::min(fit, hs) : hs;
    bounded = true;
  }
  const float scale = (bounded && fit < 1.0f) ? fit : 1.0f;
  const bool scaled = scale < 1.0f;

  // Row buffers and two dither error rows, PSRAM, freed below.
  const size_t rowBytes = static_cast<size_t>(bitmap.getRowBytes());
  auto* block = static_cast<uint8_t*>(psram(rowBytes + bw));
  auto* errors = static_cast<float*>(psram(2 * (bw + 2) * sizeof(float)));
  if (!block || !errors) {
    LOG_ERR("SG16", "no PSRAM for %dpx rows", bw);
    heap_caps_free(block);
    heap_caps_free(errors);
    return;
  }
  uint8_t* rowBuf = block;
  uint8_t* lum = block + rowBytes;
  float* errCur = errors;
  float* errNext = errors + (bw + 2);
  memset(errors, 0, 2 * (bw + 2) * sizeof(float));

  const float white = levelLstar[0], black = levelLstar[15];
  const int screenW = renderer.getScreenWidth(), screenH = renderer.getScreenHeight();
  for (int bmpY = 0; bmpY < bh - cropPixY; bmpY++) {
    int screenY = -cropPixY + (bitmap.isTopDown() ? bmpY : bh - 1 - bmpY);
    if (scaled) screenY = static_cast<int>(std::floor(screenY * scale));
    screenY += g.y;
    if (screenY >= screenH) break;
    if (bitmap.readNextRowLum(lum, rowBuf) != BmpReaderError::Ok) break;
    if (screenY < 0 || bmpY < cropPixY) continue;
    for (int bmpX = cropPixX; bmpX < bw - cropPixX; bmpX++) {
      const int idx = bmpX - cropPixX;
      int screenX = scaled ? static_cast<int>(std::floor(idx * scale)) : idx;
      screenX += g.x;
      if (screenX >= screenW) break;
      // Dither along the source row (cover BMPs are pre-scaled to the screen,
      // so source and screen pixels are close to one to one).
      const float want = black + (white - black) * (lumToLstar(lum[bmpX]) / 100.0f) + errCur[bmpX + 1];
      uint8_t best = 0;
      for (uint8_t l = 1; l < 16; ++l)
        if (fabsf(levelLstar[l] - want) < fabsf(levelLstar[best] - want)) best = l;
      const float e = want - levelLstar[best];
      errCur[bmpX + 2] += e * 7.0f / 16.0f;
      errNext[bmpX] += e * 3.0f / 16.0f;
      errNext[bmpX + 1] += e * 5.0f / 16.0f;
      errNext[bmpX + 2] += e * 1.0f / 16.0f;
      if (screenX < 0) continue;
      int px, py;
      renderer.logicalToPanel(screenX, screenY, &px, &py);
      out.set(px, py, best);
    }
    float* t = errCur;
    errCur = errNext;
    errNext = t;
    memset(errNext, 0, (bw + 2) * sizeof(float));
  }
  heap_caps_free(block);
  heap_caps_free(errors);
}

}  // namespace

// The B/W frame (panel-native, 1 bpp, cleared bit = black) as levels 15 / 0.
void stampFrame(uint8_t* levels, const uint8_t* frame, const uint16_t w, const uint16_t h) {
  const size_t rowBytes = w / 8;
  for (uint16_t y = 0; y < h; ++y) {
    const uint8_t* src = frame + y * rowBytes;
    uint8_t* dst = levels + static_cast<size_t>(y) * (w / 2);
    for (size_t xb = 0; xb < rowBytes; ++xb) {
      const uint8_t ink = static_cast<uint8_t>(~src[xb]);
      uint8_t* d = dst + xb * 4;
      d[0] = static_cast<uint8_t>(((ink & 0x80) ? 0xF0 : 0) | ((ink & 0x40) ? 0x0F : 0));
      d[1] = static_cast<uint8_t>(((ink & 0x20) ? 0xF0 : 0) | ((ink & 0x10) ? 0x0F : 0));
      d[2] = static_cast<uint8_t>(((ink & 0x08) ? 0xF0 : 0) | ((ink & 0x04) ? 0x0F : 0));
      d[3] = static_cast<uint8_t>(((ink & 0x02) ? 0xF0 : 0) | ((ink & 0x01) ? 0x0F : 0));
    }
  }
}

// levelLstar from the driver. False when it has no measured tones (a
// calibration hook replaced the paths): the caller keeps its 4-shade path.
bool loadLevelLstar() {
  const int16_t* lstar = display.gray16LevelLstar();
  if (!lstar) {
    LOG_ERR("SG16", "no measured 16-level tones");
    return false;
  }
  for (int l = 0; l < 16; ++l) levelLstar[l] = lstar[l] / 10.0f;
  return true;
}

bool present(GfxRenderer& renderer, const SleepThemeRenderer::GreyPlan& plan) {
  if (!display.supportsGray16Image()) return false;
  if (!plan.frontDir.empty()) return false;  // art in front of the images: keep the 4-shade passes
  if (!loadLevelLstar()) return false;       // keep the 4-shade passes
  const uint16_t w = display.getDisplayWidth(), h = display.getDisplayHeight();
  const size_t levelBytes = static_cast<size_t>(w / 2) * h;  // 192 KB
  const size_t frameBytes = renderer.getBufferSize();        // 48 KB
  auto* buf = static_cast<uint8_t*>(psram(levelBytes + frameBytes));
  if (!buf) {
    LOG_ERR("SG16", "no PSRAM for the 16-level sleep screen");
    return false;
  }
  const unsigned long t0 = millis();
  uint8_t* const levels = buf;
  uint8_t* const frame = buf + levelBytes;
  memcpy(frame, renderer.getFrameBuffer(), frameBytes);  // the theme's B/W frame

  // The open book's cover in full tones (may borrow the framebuffer to load
  // the book's metadata: the frame is safe in `frame`).
  std::string paths[SleepThemeRenderer::GreyPlan::MAX_IMAGES];
  for (uint8_t i = 0; i < plan.count; i++) {
    std::string gray;
    paths[i] = SleepCover::grayVersionOf(renderer, plan.images[i].path, gray) ? gray : plan.images[i].path;
  }

  // Black and white from the frame.
  stampFrame(levels, frame, w, h);
  const Levels out{levels, w, h};
  for (uint8_t i = 0; i < plan.count; i++) drawImage(renderer, out, paths[i], plan.images[i]);

  memcpy(renderer.getFrameBuffer(), frame, frameBytes);
  const unsigned long tCompose = millis();
  const bool ok = display.displayGray16Image(levels, HalDisplay::HALF_REFRESH, true);
  heap_caps_free(buf);
  shown = ok;
  LOG_INF("SG16", "16-level sleep screen: %u image(s), compose %lu ms, display %lu ms (%s)",
          static_cast<unsigned>(plan.count), tCompose - t0, millis() - tCompose, ok ? "ok" : "refused");
  return ok;
}

bool isUp() { return shown; }

bool showImage(GfxRenderer& renderer, const std::string& bmpPath) {
  if (!display.supportsGray16Image() || !loadLevelLstar()) return false;
  // Fit the picture inside the screen (never up) and centre it, as the viewer's
  // B/W path does.
  int imgW = 0, imgH = 0;
  {
    FsFile file;
    if (!Storage.openFileForRead("SG16", bmpPath, file)) return false;
    Bitmap bitmap(file, false);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
    imgW = bitmap.getWidth();
    imgH = bitmap.getHeight();
  }
  if (imgW <= 0 || imgH <= 0) return false;
  const int screenW = renderer.getScreenWidth(), screenH = renderer.getScreenHeight();
  const float scale = std::min(1.0f, std::min(static_cast<float>(screenW) / imgW, static_cast<float>(screenH) / imgH));
  SleepThemeRenderer::GreyImage g;
  g.path = bmpPath;
  g.width = static_cast<int16_t>(screenW);
  g.height = static_cast<int16_t>(screenH);
  g.x = static_cast<int16_t>((screenW - static_cast<int>(imgW * scale)) / 2);
  g.y = static_cast<int16_t>((screenH - static_cast<int>(imgH * scale)) / 2);

  const uint16_t w = display.getDisplayWidth(), h = display.getDisplayHeight();
  const size_t levelBytes = static_cast<size_t>(w / 2) * h;  // 192 KB: PSRAM, freed below
  auto* levels = static_cast<uint8_t*>(psram(levelBytes));
  if (!levels) {
    LOG_ERR("SG16", "no PSRAM for a 16-level image");
    return false;
  }
  // Whatever the caller drew (button hints) in black and white, the picture over it.
  stampFrame(levels, renderer.getFrameBuffer(), w, h);
  const Levels out{levels, w, h};
  drawImage(renderer, out, bmpPath, g);
  const unsigned long t0 = millis();
  const bool ok = display.displayGray16Image(levels, HalDisplay::HALF_REFRESH, false);
  heap_caps_free(levels);
  LOG_INF("SG16", "16-level image %s in %lu ms", ok ? "shown" : "refused", millis() - t0);
  return ok;
}

void saveOwedDrive() {
  if (!shown) return;
  const size_t size = display.gray16StateSize();
  if (size == 0) return;  // this driver owes nothing after its draw (UC8179: every pass powers off)
  // ~192 KB (the screen's levels plus a 58-byte header): PSRAM, freed below.
  auto* state = size ? static_cast<uint8_t*>(psram(size)) : nullptr;
  if (!state) {
    LOG_ERR("SG16", "no PSRAM to save the 16-level screen's drive (%u bytes)", static_cast<unsigned>(size));
    return;
  }
  const size_t len = display.exportGray16State(state, size);
  bool ok = false;
  if (len) {
    FsFile file;
    if (Storage.openFileForWrite("SG16", OWED_DRIVE_FILE, file)) {
      ok = file.write(state, len) == len;
      file.close();
    }
    if (!ok) Storage.remove(OWED_DRIVE_FILE);  // never leave a partial state for the next boot
  }
  heap_caps_free(state);
  LOG_INF("SG16", "16-level screen drive %s", ok ? "saved for the next boot" : "not saved");
}

void restoreOwedDrive() {
  if (!Storage.exists(OWED_DRIVE_FILE)) return;
  const size_t size = display.gray16StateSize();
  auto* state = size ? static_cast<uint8_t*>(psram(size)) : nullptr;
  bool ok = false;
  if (state) {
    FsFile file;
    if (Storage.openFileForRead("SG16", OWED_DRIVE_FILE, file)) {
      ok = file.fileSize() == size && file.read(state, size) == static_cast<int>(size);
      file.close();
    }
    ok = ok && display.importGray16State(state, size);
    heap_caps_free(state);
  }
  // One boot only: once imported the driver owns it, and a state it refused is stale.
  Storage.remove(OWED_DRIVE_FILE);
  if (ok) {
    LOG_INF("SG16", "16-level screen drive restored; the first frame gives it back");
  } else {
    LOG_ERR("SG16", "16-level screen drive not restored (%u bytes expected)", static_cast<unsigned>(size));
  }
}

}  // namespace SleepGray16

#endif
