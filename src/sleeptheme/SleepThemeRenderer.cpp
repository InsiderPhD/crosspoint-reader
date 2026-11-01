#include "SleepThemeRenderer.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "SleepThemeFormat.h"
#include "SleepThemeText.h"
#include "SleepThemeVars.h"
#include "activities/boot_sleep/SleepCover.h"
#include "fontIds.h"

namespace SleepThemeRenderer {

namespace {

using namespace SleepThemeFormat;

constexpr size_t PATH_LEN = 96;
constexpr size_t TEXT_LEN = 192;
// Most lines one wrapped text element draws (data-max-lines is clamped to it).
constexpr uint8_t MAX_LINES = 8;
// Largest 1-bit scratch for one transformed element: 32KB covers a 512x512
// box. Bigger boxes are drawn untransformed rather than risk the heap.
constexpr size_t MAX_SCRATCH_BYTES = 32 * 1024;
// Text falls back to this built-in font when a theme font is missing or lacks
// a glyph the live value needs.
constexpr int FALLBACK_FONT_ID = UI_12_FONT_ID;
// Theme font IDs. Arbitrary, but distinct from the FNV-hashed built-in and
// SD-font IDs (checked against the font map before use).
constexpr int THEME_FONT_ID_BASE = 0x5EE70000;

// The theme's fonts, registered with the renderer for the duration of one draw.
class ThemeFonts {
 public:
  explicit ThemeFonts(GfxRenderer& renderer) : renderer(renderer) {}
  ~ThemeFonts() {
    for (uint8_t i = 0; i < MAX_FONTS; i++) {
      if (fonts[i] == nullptr) continue;
      renderer.removeFont(ids[i]);
      delete fonts[i];
      fonts[i] = nullptr;
    }
  }
  ThemeFonts(const ThemeFonts&) = delete;
  ThemeFonts& operator=(const ThemeFonts&) = delete;

  void load(const char* dir, const uint8_t count) {
    char path[PATH_LEN];
    for (uint8_t i = 0; i < count && i < MAX_FONTS; i++) {
      snprintf(path, sizeof(path), "%s/f%u.cpfont", dir, static_cast<unsigned>(i));
      const int id = THEME_FONT_ID_BASE + i;
      if (renderer.getFontMap().count(id) != 0) {
        LOG_ERR("STH", "Theme font id %d already in use", id);
        continue;
      }
      // Heap: one SdCardFont per theme font (header + interval tables), freed
      // in the destructor. Count and lifetime are bounded by this draw.
      auto* font = new (std::nothrow) SdCardFont();
      if (font == nullptr) {
        LOG_ERR("STH", "No memory for theme font %s", path);
        continue;
      }
      if (!font->load(path)) {
        LOG_ERR("STH", "Failed to load theme font %s", path);
        delete font;
        continue;
      }
      // A theme font holds only the styles its live text uses, often just bold.
      // The renderer reads line metrics from the REGULAR slot (getLineHeight,
      // getFontAscenderSize) and getFont() falls back to it, so a null regular
      // is a null dereference. Fill it with the lowest style present.
      EpdFont* regular = nullptr;
      for (uint8_t s = 0; s < 4 && regular == nullptr; s++) regular = font->getEpdFont(s);
      if (regular == nullptr) {
        LOG_ERR("STH", "Theme font %s has no styles", path);
        delete font;
        continue;
      }
      renderer.registerSdCardFont(id, font);
      renderer.insertFont(id, EpdFontFamily(regular, font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3)));
      fonts[i] = font;
      ids[i] = id;
    }
  }

  // Font to draw text in: the theme font when it is loaded and has every
  // glyph text needs (pulling those glyphs in from SD), else the fallback.
  int prepare(const uint8_t index, const uint8_t style, const char* text) const {
    if (index >= MAX_FONTS || fonts[index] == nullptr) return FALLBACK_FONT_ID;
    if (!fonts[index]->hasStyle(style)) {
      LOG_ERR("STH", "Theme font f%u has no style %u", static_cast<unsigned>(index), static_cast<unsigned>(style));
      return FALLBACK_FONT_ID;
    }
    // prewarm() always adds U+FFFD to the set it loads, so a font without it
    // reports one miss for any text. Count that baseline first (an empty
    // string prewarms only U+FFFD), then subtract it from the text's misses.
    const uint8_t mask = static_cast<uint8_t>(1u << style);
    const int baseline = std::max(0, fonts[index]->prewarm("", mask));
    const int missed = fonts[index]->prewarm(text, mask) - baseline;
    if (missed > 0) {
      LOG_INF("STH", "Theme font f%u lacks %d glyph(s) for \"%s\"; using fallback", static_cast<unsigned>(index),
              missed, text);
      return FALLBACK_FONT_ID;
    }
    return ids[index];
  }

 private:
  GfxRenderer& renderer;
  SdCardFont* fonts[MAX_FONTS] = {};
  int ids[MAX_FONTS] = {};
};

// ---- Affine helpers (16.16 fixed point; the C3 has no FPU) -----------------

constexpr int32_t FP_ONE = 1 << 16;

int32_t fpMul(const int32_t a, const int32_t b) { return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 16); }

int32_t fpDiv(const int32_t a, const int32_t b) { return static_cast<int32_t>((static_cast<int64_t>(a) * FP_ONE) / b); }

int floorFp(const int32_t v) { return v >> 16; }

int roundFp(const int32_t v) { return (v + (FP_ONE / 2)) >> 16; }

// Matrix m with an extra local translation (dx, dy) applied first.
void translateLocal(const int32_t m[6], const int dx, const int dy, int32_t out[6]) {
  memcpy(out, m, sizeof(int32_t) * 6);
  out[4] = static_cast<int32_t>(m[4] + static_cast<int64_t>(m[0]) * dx + static_cast<int64_t>(m[2]) * dy);
  out[5] = static_cast<int32_t>(m[5] + static_cast<int64_t>(m[1]) * dx + static_cast<int64_t>(m[3]) * dy);
}

void mapPoint(const int32_t m[6], const int x, const int y, int32_t& sx, int32_t& sy) {
  sx = static_cast<int32_t>(static_cast<int64_t>(m[0]) * x + static_cast<int64_t>(m[2]) * y + m[4]);
  sy = static_cast<int32_t>(static_cast<int64_t>(m[1]) * x + static_cast<int64_t>(m[3]) * y + m[5]);
}

// Paints a w x h 1-bit image (MSB first, 1 = white, rows of rowBytes) through
// matrix m, sampling the nearest source pixel for every screen pixel the
// transformed box covers. inkOnly draws just the black source pixels, in
// inkBlack; otherwise both colours are painted (an opaque image).
void blitAffine(const GfxRenderer& renderer, const uint8_t* bits, const int w, const int h, const int rowBytes,
                const int32_t m[6], const bool inkOnly, const bool inkBlack) {
  const int32_t det = fpMul(m[0], m[3]) - fpMul(m[1], m[2]);
  if (det == 0) {
    LOG_ERR("STH", "Degenerate transform; element skipped");
    return;
  }
  // Inverse of the linear part.
  const int32_t ia = fpDiv(m[3], det);
  const int32_t ib = fpDiv(-m[1], det);
  const int32_t ic = fpDiv(-m[2], det);
  const int32_t id = fpDiv(m[0], det);

  // Screen bounding box of the four transformed corners, clipped to the screen.
  int32_t xs[4], ys[4];
  mapPoint(m, 0, 0, xs[0], ys[0]);
  mapPoint(m, w, 0, xs[1], ys[1]);
  mapPoint(m, 0, h, xs[2], ys[2]);
  mapPoint(m, w, h, xs[3], ys[3]);
  int32_t minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
  for (int i = 1; i < 4; i++) {
    minX = std::min(minX, xs[i]);
    maxX = std::max(maxX, xs[i]);
    minY = std::min(minY, ys[i]);
    maxY = std::max(maxY, ys[i]);
  }
  const int x0 = std::max(0, floorFp(minX));
  const int y0 = std::max(0, floorFp(minY));
  const int x1 = std::min(renderer.getScreenWidth() - 1, floorFp(maxX));
  const int y1 = std::min(renderer.getScreenHeight() - 1, floorFp(maxY));

  for (int sy = y0; sy <= y1; sy++) {
    // Local coordinates of this row's first pixel centre, then step by the
    // inverse matrix's first column per pixel.
    const int32_t dx = (x0 << 16) + FP_ONE / 2 - m[4];
    const int32_t dy = (sy << 16) + FP_ONE / 2 - m[5];
    int32_t lx = fpMul(ia, dx) + fpMul(ic, dy);
    int32_t ly = fpMul(ib, dx) + fpMul(id, dy);
    for (int sx = x0; sx <= x1; sx++, lx += ia, ly += ib) {
      const int px = floorFp(lx);
      const int py = floorFp(ly);
      if (px < 0 || py < 0 || px >= w || py >= h) continue;
      const bool white = (bits[py * rowBytes + (px >> 3)] & (0x80u >> (px & 7))) != 0;
      if (inkOnly) {
        if (!white) renderer.drawPixel(sx, sy, inkBlack);
      } else {
        renderer.drawPixel(sx, sy, !white);
      }
    }
  }
}

// Allocates a white w x h 1-bit scratch. Heap rather than stack: boxes are
// routinely several KB. Null (logged) when too big or out of memory.
std::unique_ptr<uint8_t[]> allocScratch(const int w, const int h, int& rowBytes) {
  rowBytes = (w + 7) / 8;
  const size_t bytes = static_cast<size_t>(rowBytes) * static_cast<size_t>(h);
  if (w <= 0 || h <= 0 || bytes > MAX_SCRATCH_BYTES) {
    LOG_ERR("STH", "Transform box %dx%d too large (%u bytes); drawing untransformed", w, h,
            static_cast<unsigned>(bytes));
    return nullptr;
  }
  auto scratch = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!scratch) {
    LOG_ERR("STH", "malloc failed: %u bytes transform scratch; drawing untransformed", static_cast<unsigned>(bytes));
    return nullptr;
  }
  memset(scratch.get(), 0xFF, bytes);
  return scratch;
}

// Rasterises one line of text into a 1-bit scratch (0 = ink), top edge at
// lineTop. Mirrors drawText's pen advance (kerning snapped per pair, tracking
// spread over glyph gaps) so a transformed line has the same shape as an
// untransformed one.
void rasterLine(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const int8_t tracking,
                const char* text, const int penX, const int lineTop, uint8_t* bits, const int w, const int h,
                const int rowBytes) {
  const auto fontIt = renderer.getFontMap().find(fontId);
  if (fontIt == renderer.getFontMap().end()) return;
  const EpdFontFamily& font = fontIt->second;
  const EpdFontData* data = font.getData(style);
  if (data == nullptr) return;
  const int baseline = lineTop + renderer.getFontAscenderSize(fontId);

  int x = penX;
  int gaps = 0;
  int32_t prevAdvance = 0;
  uint32_t prevCp = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    if (utf8IsVariationSelector(cp) || utf8IsCombiningMark(cp)) continue;
    if (prevCp != 0) {
      x += fp4::toPixel(prevAdvance + font.getKerning(prevCp, cp, style));
      if (tracking != 0 && prevCp != ' ' && cp != ' ') {
        x += GfxRenderer::trackingPixels(gaps + 1, tracking) - GfxRenderer::trackingPixels(gaps, tracking);
        gaps++;
      }
    }
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    prevCp = cp;
    prevAdvance = glyph ? glyph->advanceX : 0;
    if (glyph == nullptr) continue;
    const uint8_t* bitmap = renderer.getGlyphBitmap(data, glyph);
    if (bitmap == nullptr) continue;
    const int gx0 = x + glyph->left;
    const int gy0 = baseline - glyph->top;
    for (int gy = 0; gy < glyph->height; gy++) {
      const int ty = gy0 + gy;
      if (ty < 0 || ty >= h) continue;
      for (int gx = 0; gx < glyph->width; gx++) {
        const int tx = gx0 + gx;
        if (tx < 0 || tx >= w) continue;
        const int pos = gy * glyph->width + gx;
        // Any non-white level is ink, as drawText does in BW mode.
        const bool ink = data->is2Bit ? ((bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3) != 0
                                      : ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 0x1) != 0;
        if (ink) bits[ty * rowBytes + (tx >> 3)] &= static_cast<uint8_t>(~(0x80u >> (tx & 7)));
      }
    }
  }
}

// Keeps only the digitIndex-th digit from the right of value ("" if none).
void pickDigit(const char* value, const int8_t digitIndex, char* out, const size_t outSize) {
  out[0] = '\0';
  int seen = 0;
  for (int i = static_cast<int>(strlen(value)) - 1; i >= 0; i--) {
    if (value[i] < '0' || value[i] > '9') continue;
    if (seen++ == digitIndex) {
      snprintf(out, outSize, "%c", value[i]);
      return;
    }
  }
}

// The image value a Cover element shows ("{{book.cover}}", "{{recent.3.cover}}").
bool coverVar(const char* tpl, SleepThemeVars::Id& id) {
  const char* open = strstr(tpl, "{{");
  if (open == nullptr) return false;
  const char* name = open + 2;
  const char* close = strstr(name, "}}");
  if (close == nullptr) return false;
  const char* colon = static_cast<const char*>(memchr(name, ':', static_cast<size_t>(close - name)));
  return SleepThemeVars::find(name, static_cast<size_t>((colon ? colon : close) - name), id);
}

class Painter {
 public:
  Painter(GfxRenderer& renderer, const ThemeFonts& fonts, const char* strings, const uint16_t stringsSize,
          const std::string& coverPath)
      : renderer(renderer), fonts(fonts), strings(strings), stringsSize(stringsSize), coverPath(coverPath) {}

  void paint(const Element& e) const {
    switch (static_cast<ElementKind>(e.kind)) {
      case ElementKind::Text:
        return paintText(e);
      case ElementKind::Cover:
        return paintCover(e);
      case ElementKind::Bar:
        return paintBar(e);
    }
    LOG_ERR("STH", "Unknown element kind %u", static_cast<unsigned>(e.kind));
  }

 private:
  const char* templateOf(const Element& e) const {
    if (e.templateOffset == NO_STRING || e.templateOffset >= stringsSize) return "";
    return strings + e.templateOffset;
  }

  void paintText(const Element& e) const {
    char value[TEXT_LEN];
    const bool allPresent = SleepThemeVars::expand(templateOf(e), value, sizeof(value));
    if ((e.flags & FLAG_HIDE_IF_EMPTY) && !allPresent) return;

    char picked[TEXT_LEN];
    const char* source = value;
    if (e.digitIndex >= 0) {
      pickDigit(value, e.digitIndex, picked, sizeof(picked));
      source = picked;
    }
    char text[TEXT_LEN];
    SleepThemeText::applyTransform(static_cast<TextTransform>(e.textTransform), source, text, sizeof(text));
    if (text[0] == '\0') return;

    const auto style = static_cast<EpdFontFamily::Style>(e.fontStyle & 0x03);
    const int fontId = fonts.prepare(e.fontIndex, e.fontStyle & 0x03, text);
    const int8_t tracking = e.tracking;

    // Lines: one (optionally ellipsised) or word-wrapped up to maxLines.
    std::vector<std::string> lines;
    if (e.maxLines > 1) {
      lines = renderer.wrappedText(fontId, text, e.width, std::min(e.maxLines, MAX_LINES), style);
    } else if ((e.flags & FLAG_ELLIPSIS) && renderer.getTextAdvanceX(fontId, text, style, tracking) > e.width) {
      lines.push_back(renderer.truncatedText(fontId, text, e.width, style));
    } else {
      lines.emplace_back(text);
    }

    const int fontLineH = renderer.getLineHeight(fontId);
    const int lineH = e.lineHeight > 0 ? e.lineHeight : fontLineH;
    // CSS half-leading: the font's line box sits centred in each line.
    const int leading = (lineH - fontLineH) / 2;
    const bool inkBlack = (e.flags & FLAG_WHITE) == 0;

    // x of a line of width tw inside a box boxW wide.
    auto alignX = [&](const int tw, const int boxW) {
      switch (static_cast<TextAlign>(e.align)) {
        case TextAlign::Center:
          return (boxW - tw) / 2;
        case TextAlign::Right:
          return boxW - tw;
        case TextAlign::Left:
          break;
      }
      return 0;
    };
    int widths[MAX_LINES];
    const size_t lineCount = std::min<size_t>(lines.size(), MAX_LINES);
    int widest = 0;
    for (size_t i = 0; i < lineCount; i++) {
      widths[i] = renderer.getTextAdvanceX(fontId, lines[i].c_str(), style, tracking);
      widest = std::max(widest, widths[i]);
    }

    if (e.flags & FLAG_TRANSFORMED) {
      // The box was measured around a sample; a wider live value grows it
      // the way its alignment says (both ways when centred) instead of clipping.
      const int boxW = std::max<int>(e.width, widest);
      // Likewise tall enough for the font actually drawn: the fallback UI
      // font can be taller than the theme font the box was measured with.
      const int boxH =
          std::max<int>(e.height, static_cast<int>(lineCount - 1) * lineH + std::max(0, leading) + fontLineH);
      int rowBytes = 0;
      auto scratch = allocScratch(boxW, boxH, rowBytes);
      if (scratch) {
        for (size_t i = 0; i < lineCount; i++) {
          rasterLine(renderer, fontId, style, tracking, lines[i].c_str(), alignX(widths[i], boxW),
                     static_cast<int>(i) * lineH + leading, scratch.get(), boxW, boxH, rowBytes);
        }
        int32_t m[6];
        translateLocal(e.matrix, -alignX(e.width, boxW), 0, m);
        blitAffine(renderer, scratch.get(), boxW, boxH, rowBytes, m, /*inkOnly=*/true, inkBlack);
        return;
      }
      // Fall through: draw upright at the box origin.
    }

    const int originX = roundFp(e.matrix[4]);
    const int originY = roundFp(e.matrix[5]);
    for (size_t i = 0; i < lineCount; i++) {
      renderer.drawText(fontId, originX + alignX(widths[i], e.width), originY + static_cast<int>(i) * lineH + leading,
                        lines[i].c_str(), inkBlack, style, tracking);
    }
  }

  void paintCover(const Element& e) const {
    // book.cover was resolved before painting; a recent.N.cover is an image
    // already cached on the SD card, looked up here.
    std::string recentPath;
    SleepThemeVars::Id id;
    const bool isRecent = coverVar(templateOf(e), id) && id != SleepThemeVars::Id::BookCover;
    if (isRecent && !SleepThemeVars::recentCoverPath(id, recentPath)) return;
    const std::string& path = isRecent ? recentPath : coverPath;
    if (path.empty()) return;
    LOG_DBG("STH", "Cover %s: %s", templateOf(e), path.c_str());
    FsFile file;
    if (!Storage.openFileForRead("STH", path, file)) {
      LOG_ERR("STH", "Could not open cover %s", path.c_str());
      return;
    }
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
      LOG_ERR("STH", "Unusable cover bmp %s", path.c_str());
      return;
    }

    // object-fit: contain keeps the aspect ratio inside the box; otherwise the
    // (already cropped) cover is stretched to fill it.
    int cx = 0, cy = 0, cw = e.width, ch = e.height;
    if (e.flags & FLAG_CONTAIN) {
      if (static_cast<int64_t>(bitmap.getWidth()) * e.height > static_cast<int64_t>(bitmap.getHeight()) * e.width) {
        ch = static_cast<int>(static_cast<int64_t>(bitmap.getHeight()) * e.width / bitmap.getWidth());
        cy = (e.height - ch) / 2;
      } else {
        cw = static_cast<int>(static_cast<int64_t>(bitmap.getWidth()) * e.height / bitmap.getHeight());
        cx = (e.width - cw) / 2;
      }
    }
    if (cw <= 0 || ch <= 0) return;

    if (e.flags & FLAG_TRANSFORMED) {
      int rowBytes = 0;
      auto scratch = allocScratch(cw, ch, rowBytes);
      if (scratch && GfxRenderer::resampleBitmapTo1Bit(bitmap, cw, ch, scratch.get(), rowBytes)) {
        int32_t m[6];
        translateLocal(e.matrix, cx, cy, m);
        blitAffine(renderer, scratch.get(), cw, ch, rowBytes, m, /*inkOnly=*/false, true);
        return;
      }
      // Fall through: draw upright at the box origin.
    }

    const int x = roundFp(e.matrix[4]) + cx;
    const int y = roundFp(e.matrix[5]) + cy;
    // drawBitmapResampled only paints black, so clear the box first.
    renderer.fillRect(x, y, cw, ch, false);
    renderer.drawBitmapResampled(bitmap, x, y, cw, ch);
  }

  void paintBar(const Element& e) const {
    // The template should reference one numeric value (0-100).
    char value[TEXT_LEN];
    if (!SleepThemeVars::expand(templateOf(e), value, sizeof(value)) || value[0] == '\0') return;
    // Any number can drive a bar: barMax is the value that fills it.
    const int64_t full = e.barMax > 0 ? e.barMax : 100;
    int64_t pct = strtoll(value, nullptr, 10) * 100 / full;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    const int fillW = static_cast<int>(e.width * pct / 100);
    if (fillW <= 0) return;
    const bool black = (e.flags & FLAG_WHITE) == 0;

    if (e.flags & FLAG_TRANSFORMED) {
      int32_t xs[4], ys[4];
      mapPoint(e.matrix, 0, 0, xs[0], ys[0]);
      mapPoint(e.matrix, fillW, 0, xs[1], ys[1]);
      mapPoint(e.matrix, fillW, e.height, xs[2], ys[2]);
      mapPoint(e.matrix, 0, e.height, xs[3], ys[3]);
      int px[4], py[4];
      for (int i = 0; i < 4; i++) {
        px[i] = roundFp(xs[i]);
        py[i] = roundFp(ys[i]);
      }
      renderer.fillPolygon(px, py, 4, black);
      return;
    }
    renderer.fillRect(roundFp(e.matrix[4]), roundFp(e.matrix[5]), fillW, e.height, black);
  }

  GfxRenderer& renderer;
  const ThemeFonts& fonts;
  const char* strings;
  uint16_t stringsSize;
  const std::string& coverPath;
};

bool readExact(FsFile& file, void* dst, const size_t n) { return file.read(dst, n) == static_cast<int>(n); }

bool drawBackground(const GfxRenderer& renderer, const char* dir, const Header& header) {
  char path[PATH_LEN];
  snprintf(path, sizeof(path), "%s/bg.bmp", dir);
  renderer.clearScreen();
  FsFile file;
  if (!Storage.openFileForRead("STH", path, file)) {
    LOG_ERR("STH", "Missing background %s", path);
    return false;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || !bitmap.is1Bit() || bitmap.getWidth() != header.screenWidth ||
      bitmap.getHeight() != header.screenHeight) {
    LOG_ERR("STH", "Background %s must be a %ux%u 1-bit BMP", path, header.screenWidth, header.screenHeight);
    return false;
  }
  renderer.drawBitmap(bitmap, 0, 0, header.screenWidth, header.screenHeight);
  return true;
}

}  // namespace

bool draw(GfxRenderer& renderer, const char* themeName) {
  if (themeName == nullptr || themeName[0] == '\0' || strchr(themeName, '/') != nullptr) {
    LOG_ERR("STH", "No usable theme name set");
    return false;
  }
  char dir[PATH_LEN];
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, themeName);
  char path[PATH_LEN];
  snprintf(path, sizeof(path), "%s/theme.bin", dir);

  FsFile file;
  if (!Storage.openFileForRead("STH", path, file)) {
    LOG_ERR("STH", "Missing %s", path);
    return false;
  }
  Header header;
  if (!readExact(file, &header, sizeof(header)) || memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0) {
    LOG_ERR("STH", "%s is not a compiled theme", path);
    return false;
  }
  if (header.version != VERSION) {
    LOG_ERR("STH", "%s has version %u, firmware reads %u; recompile it", path, header.version, VERSION);
    return false;
  }
  if (header.elementCount > MAX_ELEMENTS || header.fontCount > MAX_FONTS || header.stringTableSize > MAX_STRING_TABLE ||
      header.orientation > 3) {
    LOG_ERR("STH", "%s header out of range", path);
    return false;
  }

  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(header.orientation));
  if (renderer.getScreenWidth() != header.screenWidth || renderer.getScreenHeight() != header.screenHeight) {
    LOG_ERR("STH", "Theme laid out for %ux%u, screen is %dx%d", header.screenWidth, header.screenHeight,
            renderer.getScreenWidth(), renderer.getScreenHeight());
    return false;
  }

  // Heap: the layout (<= 64 x 48 B elements + <= 4KB strings) for this draw
  // only. Static storage would hold ~7KB of DRAM for the whole uptime.
  const size_t elementBytes = static_cast<size_t>(header.elementCount) * ELEMENT_SIZE;
  auto layout = makeUniqueNoThrow<uint8_t[]>(elementBytes + header.stringTableSize + 1);
  if (!layout) {
    LOG_ERR("STH", "malloc failed: %u bytes theme layout",
            static_cast<unsigned>(elementBytes + header.stringTableSize));
    return false;
  }
  if (!readExact(file, layout.get(), elementBytes + header.stringTableSize)) {
    LOG_ERR("STH", "%s is truncated", path);
    return false;
  }
  file.close();
  char* strings = reinterpret_cast<char*>(layout.get() + elementBytes);
  strings[header.stringTableSize] = '\0';

  LOG_DBG("STH", "Theme %s: %u elements, %u fonts; heap free=%u largest=%u", themeName, header.elementCount,
          header.fontCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // The cover lookup may borrow the framebuffer, so it runs before any paint.
  std::string coverPath;
  for (uint8_t i = 0; i < header.elementCount; i++) {
    Element e;
    memcpy(&e, layout.get() + static_cast<size_t>(i) * ELEMENT_SIZE, sizeof(e));
    // A cover with no template is book.cover: themes compiled before
    // recent.N.cover existed stored none.
    SleepThemeVars::Id id;
    const char* tpl = e.templateOffset < header.stringTableSize ? strings + e.templateOffset : "";
    const bool bookCover = !coverVar(tpl, id) || id == SleepThemeVars::Id::BookCover;
    if (static_cast<ElementKind>(e.kind) == ElementKind::Cover && (header.flags & HEADER_FLAG_COVER_TEMPLATES) == 0) {
      LOG_INF("STH",
              "Theme %s was compiled before covers named their book: every cover shows the open book. "
              "Recompile it on the Sleep Themes page.",
              themeName);
    }
    if (static_cast<ElementKind>(e.kind) == ElementKind::Cover && bookCover) {
      if (!SleepCover::resolveBmpPath(renderer, (e.flags & FLAG_CONTAIN) == 0, coverPath)) coverPath.clear();
      break;
    }
  }

  if (!drawBackground(renderer, dir, header)) return false;

  ThemeFonts fonts(renderer);
  fonts.load(dir, header.fontCount);
  SleepThemeVars::beginFrame();

  const Painter painter(renderer, fonts, strings, header.stringTableSize, coverPath);
  for (uint8_t i = 0; i < header.elementCount; i++) {
    Element e;
    memcpy(&e, layout.get() + static_cast<size_t>(i) * ELEMENT_SIZE, sizeof(e));
    painter.paint(e);
  }
  SleepThemeVars::endFrame();

  LOG_DBG("STH", "Theme drawn; heap free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

}  // namespace SleepThemeRenderer
