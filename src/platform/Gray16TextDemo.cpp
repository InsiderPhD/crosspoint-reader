#include "Gray16TextDemo.h"

#if FREEINK_DEVICE_PICCO && defined(ENABLE_SERIAL_LOG)

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

namespace {
void* demoAlloc(const size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void demoFree(void* block) { heap_caps_free(block); }
}  // namespace

// A private stb_truetype for this dev page (TtfEpdFont keeps its own copy
// private too, and packs coverage to 2 bits, which is exactly what this page
// is here to compare against). PSRAM allocator, asserts off: a font file is
// user data.
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), demoAlloc(x))
#define STBTT_free(x, u) ((void)(u), demoFree(x))
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "../../lib/EpdFont/third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace gray16demo {
namespace {

constexpr uint32_t MAX_FONT_BYTES = 3u * 1024u * 1024u;  // as TtfEpdFont::MAX_FILE_BYTES
constexpr int GLYPH_MAX = 160;                            // scratch side for one glyph's coverage
constexpr float DPI = 150.0f;                             // reader font sizes: points at 150 DPI

// The paragraph shown three times. Mixed stems, bowls, diagonals and serifs.
constexpr const char* kSample =
    "Sphinx of black quartz, judge my vow. The quick brown fox jumps over the lazy dog; "
    "pack my box with five dozen liquor jugs.";

struct Canvas {
  uint8_t* levels;
  uint16_t panelW, panelH;
  // Portrait: the device is held with the panel's native x running down the
  // page and native y running right to left (as in the calibration photos).
  int width() const { return panelH; }
  int height() const { return panelW; }
  void put(int px, int py, uint8_t level) const {
    if (px < 0 || py < 0 || px >= width() || py >= height() || level == 0) return;
    const int nx = py;
    const int ny = panelH - 1 - px;
    uint8_t& b = levels[ny * (panelW / 2) + nx / 2];
    const bool high = (nx & 1) == 0;
    const uint8_t cur = high ? (b >> 4) : (b & 15);
    if (level <= cur) return;  // darkest wins where glyphs overlap
    b = high ? static_cast<uint8_t>((b & 0x0F) | level << 4) : static_cast<uint8_t>((b & 0xF0) | level);
  }
  void fill(int x0, int y0, int w, int h, uint8_t level) const {
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x0 + w; ++x) put(x, y, level);
  }
};

float lstarToY(float L) {
  const float f = (L + 16.0f) / 116.0f;
  return f > 6.0f / 29.0f ? f * f * f : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (f - 4.0f / 29.0f);
}
float yToLstar(float Y) {
  const float f = Y > 0.008856f ? cbrtf(Y) : Y / (3.0f * (6.0f / 29.0f) * (6.0f / 29.0f)) + 4.0f / 29.0f;
  return 116.0f * f - 16.0f;
}

// coverage (0..255) -> level, choosing among `allowed` levels the one nearest
// in L* to the paper/ink mix the coverage stands for (linear in reflectance).
void buildCoverageMap(uint8_t out[256], const float L[16], const uint8_t* allowed, int nAllowed) {
  const float yWhite = lstarToY(L[0]);
  const float yInk = lstarToY(L[15]);
  for (int c = 0; c < 256; ++c) {
    const float target = yToLstar(yWhite - (yWhite - yInk) * (c / 255.0f));
    uint8_t best = allowed[0];
    for (int i = 1; i < nAllowed; ++i)
      if (fabsf(L[allowed[i]] - target) < fabsf(L[best] - target)) best = allowed[i];
    out[c] = best;
  }
}

bool hasFontExt(const char* name) {
  const size_t n = strlen(name);
  if (n < 4) return false;
  const char* e = name + n - 4;
  return !strcasecmp(e, ".ttf") || !strcasecmp(e, ".otf");
}

// First .ttf/.otf in a reader font root (/fonts, then the hidden /.fonts),
// directly or one level down (family folders). .cpfont files are skipped:
// they are pre-rasterised to 2 bits, with no coverage left to map.
bool findFontIn(const char* root, char* out, size_t len) {
  HalFile dir = Storage.open(root);
  if (!dir || !dir.isDirectory()) return false;
  char name[128];
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    if (!f.isDirectory() && hasFontExt(name)) {
      snprintf(out, len, "%s/%s", root, name);
      return true;
    }
    if (f.isDirectory() && name[0] != '.') {
      char sub[160];
      snprintf(sub, sizeof(sub), "%s/%s", root, name);
      HalFile d2 = Storage.open(sub);
      char inner[128];
      for (HalFile g = d2.openNextFile(); g; g = d2.openNextFile()) {
        g.getName(inner, sizeof(inner));
        if (!g.isDirectory() && hasFontExt(inner)) {
          snprintf(out, len, "%s/%s", sub, inner);
          return true;
        }
      }
    }
  }
  return false;
}

bool findFont(char* out, size_t len) { return findFontIn("/fonts", out, len) || findFontIn("/.fonts", out, len); }

struct Font {
  stbtt_fontinfo info{};
  float scale = 0;
  int ascent = 0, descent = 0, lineGap = 0;
  float lineHeight() const { return (ascent - descent + lineGap) * scale; }
};

// Width of text[0..n) at this size, with kerning.
float measure(const Font& f, const char* text, int n) {
  float w = 0;
  for (int i = 0; i < n; ++i) {
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f.info, text[i], &adv, &lsb);
    w += adv * f.scale;
    if (i + 1 < n) w += stbtt_GetCodepointKernAdvance(&f.info, text[i], text[i + 1]) * f.scale;
  }
  return w;
}

void drawRun(const Canvas& cv, const Font& f, const char* text, int n, float x, float baseline, const uint8_t map[256],
             uint8_t* scratch) {
  for (int i = 0; i < n; ++i) {
    const int cp = static_cast<unsigned char>(text[i]);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
    const float shift = x - floorf(x);
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBoxSubpixel(&f.info, cp, f.scale, f.scale, shift, 0, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w <= GLYPH_MAX && h <= GLYPH_MAX) {
      stbtt_MakeCodepointBitmapSubpixel(&f.info, scratch, w, h, w, f.scale, f.scale, shift, 0, cp);
      const int ox = static_cast<int>(floorf(x)) + x0;
      const int oy = static_cast<int>(lroundf(baseline)) + y0;
      for (int gy = 0; gy < h; ++gy)
        for (int gx = 0; gx < w; ++gx) cv.put(ox + gx, oy + gy, map[scratch[gy * w + gx]]);
    }
    x += adv * f.scale;
    if (i + 1 < n) x += stbtt_GetCodepointKernAdvance(&f.info, cp, static_cast<unsigned char>(text[i + 1])) * f.scale;
  }
}

// Greedy word wrap into `width`; returns the y below the last line drawn.
float drawParagraph(const Canvas& cv, const Font& f, const char* text, float x, float y, float width, int maxLines,
                    const uint8_t map[256], uint8_t* scratch) {
  const int len = static_cast<int>(strlen(text));
  int start = 0;
  for (int line = 0; line < maxLines && start < len; ++line) {
    int end = start, lastFit = -1;
    while (end <= len) {
      if (end == len || text[end] == ' ') {
        if (measure(f, text + start, end - start) > width) break;
        lastFit = end;
      }
      ++end;
    }
    if (lastFit < 0) lastFit = end;  // one word wider than the line: draw it anyway
    y += f.ascent * f.scale;
    drawRun(cv, f, text + start, lastFit - start, x, y, map, scratch);
    y += (-f.descent + f.lineGap) * f.scale;
    start = lastFit + 1;
  }
  return y;
}

}  // namespace

bool render(uint8_t* levels, uint16_t panelW, uint16_t panelH, const char* fontPath, const float levelLstar[16]) {
  char found[200];
  if (!fontPath || !*fontPath) {
    if (!findFont(found, sizeof(found))) {
      LOG_ERR("G16", "no .ttf/.otf in /fonts or /.fonts; copy one there or pass a path");
      return false;
    }
    fontPath = found;
  }
  HalFile file;
  if (!Storage.openFileForRead("G16", fontPath, file)) return false;
  const size_t bytes = file.size();
  if (bytes == 0 || bytes > MAX_FONT_BYTES) {
    LOG_ERR("G16", "font %s is %u bytes (max %u)", fontPath, static_cast<unsigned>(bytes),
            static_cast<unsigned>(MAX_FONT_BYTES));
    file.close();
    return false;
  }
  // The rasteriser reads the whole file at random: it must be resident. PSRAM,
  // freed before returning.
  auto* data = static_cast<uint8_t*>(demoAlloc(bytes));
  auto* scratch = static_cast<uint8_t*>(demoAlloc(GLYPH_MAX * GLYPH_MAX));
  if (!data || !scratch) {
    LOG_ERR("G16", "PSRAM alloc failed for font (%u bytes)", static_cast<unsigned>(bytes));
    demoFree(data);
    demoFree(scratch);
    file.close();
    return false;
  }
  const bool readOk = file.read(data, bytes) == static_cast<int>(bytes);
  file.close();
  Font body, small;
  const int offset = readOk ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
  if (offset < 0 || !stbtt_InitFont(&body.info, data, offset)) {
    LOG_ERR("G16", "%s is not a font stb_truetype can read", fontPath);
    demoFree(data);
    demoFree(scratch);
    return false;
  }
  small.info = body.info;
  stbtt_GetFontVMetrics(&body.info, &body.ascent, &body.descent, &body.lineGap);
  small.ascent = body.ascent;
  small.descent = body.descent;
  small.lineGap = body.lineGap;
  body.scale = stbtt_ScaleForMappingEmToPixels(&body.info, 14.0f * DPI / 72.0f);
  small.scale = stbtt_ScaleForMappingEmToPixels(&small.info, 9.0f * DPI / 72.0f);

  memset(levels, 0, static_cast<size_t>(panelW / 2) * panelH);
  const Canvas cv{levels, panelW, panelH};

  static constexpr uint8_t kAll[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  static constexpr uint8_t kFour[4] = {0, 5, 10, 15};
  static constexpr uint8_t kTwo[2] = {0, 15};
  uint8_t map16[256], map4[256], map2[256];
  buildCoverageMap(map16, levelLstar, kAll, 16);
  buildCoverageMap(map4, levelLstar, kFour, 4);
  buildCoverageMap(map2, levelLstar, kTwo, 2);

  const int margin = 24;
  const int W = cv.width();
  // The 16 levels, 0 (white, outlined) .. 15, four to a row.
  const int gap = 8, pw = (W - 2 * margin - 3 * gap) / 4, ph = 44;
  for (int i = 0; i < 16; ++i) {
    const int x = margin + (i % 4) * (pw + gap), y = margin + (i / 4) * (ph + gap);
    cv.fill(x, y, pw, ph, static_cast<uint8_t>(i));
    if (i == 0) {
      cv.fill(x, y, pw, 1, 15);
      cv.fill(x, y + ph - 1, pw, 1, 15);
      cv.fill(x, y, 1, ph, 15);
      cv.fill(x + pw - 1, y, 1, ph, 15);
    }
  }
  float y = margin + 4 * (ph + gap) + 6;
  struct Section {
    const char* label;
    const uint8_t* map;
  };
  const Section sections[] = {{"16 levels", map16}, {"4 levels", map4}, {"2 levels (no anti-aliasing)", map2}};
  for (const auto& s : sections) {
    y = drawParagraph(cv, small, s.label, margin, y, W - 2 * margin, 1, map16, scratch);
    y = drawParagraph(cv, body, kSample, margin, y + 2, W - 2 * margin, 4, s.map, scratch) + 12;
  }
  LOG_INF("G16", "text demo rendered from %s (page ends at y=%d of %d)", fontPath, static_cast<int>(y), cv.height());
  demoFree(data);
  demoFree(scratch);
  return true;
}

}  // namespace gray16demo
#endif
