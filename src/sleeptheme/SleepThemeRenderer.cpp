#include "SleepThemeRenderer.h"

#include <Bitmap.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <GlyphBitmap.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SleepImage.h"
#include "SleepThemeFormat.h"
#include "SleepThemeText.h"
#include "SleepThemeVars.h"
#include "activities/boot_sleep/SleepCover.h"
#include "fontIds.h"
#include "images/Logo120.h"

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
      if (tracking != 0) {  // every boundary, spaces too (CSS; see GfxRenderer::setTrackSpaces)
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

// Turns on GfxRenderer's CSS letter spacing for its lifetime.
struct TrackSpacesScope {
  explicit TrackSpacesScope(GfxRenderer& renderer) : renderer(renderer) { renderer.setTrackSpaces(true); }
  ~TrackSpacesScope() { renderer.setTrackSpaces(false); }
  TrackSpacesScope(const TrackSpacesScope&) = delete;
  TrackSpacesScope& operator=(const TrackSpacesScope&) = delete;
  GfxRenderer& renderer;
};

// Where the next item of a wrapping row of fitted boxes goes (FLAG_FLOW).
struct FlowCursor {
  uint8_t group = 0;  // 0 = none yet; compiled groups start at 1
  int x = 0;          // the row's used width
  int row = 0;
  bool full = false;  // out of rows: the group's remaining items are dropped
};

class Painter {
 public:
  Painter(GfxRenderer& renderer, const ThemeFonts& fonts, const char* strings, const uint16_t stringsSize,
          const std::string& coverPath, const std::string& sleepImagePath, GreyPlan* grey)
      : renderer(renderer),
        fonts(fonts),
        strings(strings),
        stringsSize(stringsSize),
        coverPath(coverPath),
        sleepImagePath(sleepImagePath),
        grey(grey) {}

  void paint(const Element& e) const {
    switch (static_cast<ElementKind>(e.kind)) {
      case ElementKind::Text:
        return paintText(e);
      case ElementKind::Cover:
        return paintCover(e);
      case ElementKind::Bar:
        return paintBar(e);
      case ElementKind::Box:
        LOG_ERR("STH", "Fitted box without a text after it");
        return;
    }
    LOG_ERR("STH", "Unknown element kind %u", static_cast<unsigned>(e.kind));
  }

  // data-fit: box's border and fill, as wide as text's live value (plus the
  // insets) and never wider than the box was laid out.
  void paintBox(const Element& box, const Element& text) const {
    int textW;
    if (!fittedTextWidth(text, textW)) return;
    const int w = std::min<int>(box.width, box.textTransform + textW + box.maxLines);
    int x = roundFp(box.matrix[4]);
    switch (static_cast<TextAlign>(box.align)) {
      case TextAlign::Center:
        x += (box.width - w) / 2;
        break;
      case TextAlign::Right:
        x += box.width - w;
        break;
      case TextAlign::Left:
        break;
    }
    drawBox(box, x, roundFp(box.matrix[5]), w);
  }

  // A FLAG_FLOW box and its text: placed after the group's previous item,
  // wrapping to a new row when the row is full. Draws the text too (the
  // caller skips it), moved by as much as its box moved.
  void paintFlowItem(const Element& box, const Element& text, FlowCursor& flow) const {
    const auto group = static_cast<uint8_t>(box.digitIndex);
    if (flow.group != group) flow = FlowCursor{group, 0, 0, false};
    if (flow.full) return;
    int textW;
    if (!fittedTextWidth(text, textW)) return;

    const int rowW = box.templateOffset;
    const int w = std::min({static_cast<int>(box.width), rowW, box.textTransform + textW + box.maxLines});
    int x = flow.x;
    if (x > 0) {
      if (x + box.coverFilter + w > rowW) {
        x = 0;
        flow.row++;
        const auto maxRows = static_cast<uint8_t>(box.tracking);
        if (maxRows > 0 && flow.row >= maxRows) {
          flow.full = true;
          return;
        }
      } else {
        x += box.coverFilter;
      }
    }
    const int bx = box.lineHeight + x;
    const int by = box.barMax + flow.row * (box.height + box.reserved[0]);
    drawBox(box, bx, by, w);
    flow.x = x + w;

    Element placed = text;
    placed.matrix[4] = text.matrix[4] - box.matrix[4] + bx * FP_ONE;
    placed.matrix[5] = text.matrix[5] - box.matrix[5] + by * FP_ONE;
    placed.align = static_cast<uint8_t>(TextAlign::Left);
    placed.width = static_cast<uint16_t>(std::max(1, w - box.textTransform - box.maxLines));
    paintText(placed);
  }

 private:
  // The width a fitted box's text will draw at (clipped to its text box, as
  // paintText ellipsises it); false when the text is hidden.
  bool fittedTextWidth(const Element& text, int& textW) const {
    if (static_cast<ElementKind>(text.kind) != ElementKind::Text) return false;
    char value[TEXT_LEN];
    const bool allPresent = SleepThemeVars::expand(templateOf(text), value, sizeof(value));
    if ((text.flags & FLAG_HIDE_IF_EMPTY) && !allPresent) return false;
    char picked[TEXT_LEN];
    const char* source = value;
    if (text.digitIndex >= 0) {
      pickDigit(value, text.digitIndex, picked, sizeof(picked));
      source = picked;
    }
    char shown[TEXT_LEN];
    SleepThemeText::applyTransform(static_cast<TextTransform>(text.textTransform), source, shown, sizeof(shown));
    if (shown[0] == '\0' && (text.flags & FLAG_HIDE_IF_EMPTY)) return false;

    const auto style = static_cast<EpdFontFamily::Style>(text.fontStyle & 0x03);
    const int fontId = fonts.prepare(text.fontIndex, text.fontStyle & 0x03, shown);
    textW = std::max(0, std::min<int>(renderer.getTextAdvanceX(fontId, shown, style, text.tracking), text.width));
    return true;
  }

  void drawBox(const Element& box, const int x, const int y, const int w) const {
    const int h = box.height;
    const int radius = std::min<int>(box.fontStyle, std::min(w, h) / 2);
    switch (static_cast<BoxFill>(box.verticalAlign)) {
      case BoxFill::Black:
        renderer.fillRoundedRect(x, y, w, h, radius, Color::Black);
        break;
      case BoxFill::White:
        renderer.fillRoundedRect(x, y, w, h, radius, Color::White);
        break;
      case BoxFill::None:
        break;
    }
    if (box.fontIndex > 0) {
      renderer.drawRoundedRect(x, y, w, h, box.fontIndex, radius, (box.flags & FLAG_WHITE) == 0);
    }
  }

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

    // Lines fewer than the box was laid out for sit where data-valign puts
    // them, so a box sized for two lines keeps one line against its bottom.
    const int textH = static_cast<int>(lineCount) * lineH;
    int top = 0;
    switch (static_cast<VerticalAlign>(e.verticalAlign)) {
      case VerticalAlign::Middle:
        top = std::max(0, (static_cast<int>(e.height) - textH) / 2);
        break;
      case VerticalAlign::Bottom:
        top = std::max(0, static_cast<int>(e.height) - textH);
        break;
      case VerticalAlign::Top:
        break;
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
                     top + static_cast<int>(i) * lineH + leading, scratch.get(), boxW, boxH, rowBytes);
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
      renderer.drawText(fontId, originX + alignX(widths[i], e.width),
                        originY + top + static_cast<int>(i) * lineH + leading, lines[i].c_str(), inkBlack, style,
                        tracking);
    }
  }

  static GfxRenderer::ResampleFilter resampleFilterOf(const Element& e) {
    switch (static_cast<CoverFilter>(e.coverFilter)) {
      case CoverFilter::Lighter:
        return GfxRenderer::ResampleFilter::Lighter;
      case CoverFilter::Darker:
        return GfxRenderer::ResampleFilter::Darker;
      case CoverFilter::BlackAndWhite:
        return GfxRenderer::ResampleFilter::Threshold;
      case CoverFilter::Invert:
        return GfxRenderer::ResampleFilter::Invert;
      case CoverFilter::Dither:
        break;
    }
    return GfxRenderer::ResampleFilter::Dither;
  }

  void paintCover(const Element& e) const {
    // book.cover and sleep.image were resolved before painting; a
    // recent.N.cover is an image already cached on the SD card, looked up here.
    std::string recentPath;
    SleepThemeVars::Id id;
    const bool named = coverVar(templateOf(e), id);
    const bool isSleepImage = named && id == SleepThemeVars::Id::SleepImage;
    const bool isRecent = named && !isSleepImage && id != SleepThemeVars::Id::BookCover;
    if (isRecent && !SleepThemeVars::recentCoverPath(id, recentPath)) return;
    const std::string& path = isSleepImage ? sleepImagePath : isRecent ? recentPath : coverPath;
    if (path.empty()) return;
    LOG_DBG("STH", "Cover %s: %s", templateOf(e), path.c_str());
    FsFile file;
    if (!Storage.openFileForRead("STH", path, file)) {
      LOG_ERR("STH", "Could not open cover %s", path.c_str());
      return;
    }
    // A sleep image is whatever the user put on the card (24-bit photos
    // included), so it is error-diffused down to the panel's levels, as the old
    // Custom sleep screen did. Covers are cached in the panel's greys already.
    Bitmap bitmap(file, /*dithering=*/isSleepImage);
    if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0) {
      LOG_ERR("STH", "Unusable cover bmp %s", path.c_str());
      return;
    }

    // object-fit: contain keeps the aspect ratio inside the box; cover (zoom)
    // fills the box and crops the middle of the image to its shape; otherwise
    // the image is stretched to fill it.
    int cx = 0, cy = 0, cw = e.width, ch = e.height;
    GfxRenderer::SourceRect source{};  // whole image
    if (e.flags & FLAG_ZOOM) {
      const int64_t srcW = bitmap.getWidth();
      const int64_t srcH = bitmap.getHeight();
      if (srcW * e.height > srcH * e.width) {
        source.width = std::max<int>(1, static_cast<int>(srcH * e.width / e.height));
        source.height = static_cast<int>(srcH);
        source.x = (static_cast<int>(srcW) - source.width) / 2;
      } else {
        source.width = static_cast<int>(srcW);
        source.height = std::max<int>(1, static_cast<int>(srcW * e.height / e.width));
        source.y = (static_cast<int>(srcH) - source.height) / 2;
      }
    } else if (e.flags & FLAG_CONTAIN) {
      if (static_cast<int64_t>(bitmap.getWidth()) * e.height > static_cast<int64_t>(bitmap.getHeight()) * e.width) {
        ch = static_cast<int>(static_cast<int64_t>(bitmap.getHeight()) * e.width / bitmap.getWidth());
        cy = (e.height - ch) / 2;
      } else {
        cw = static_cast<int>(static_cast<int64_t>(bitmap.getWidth()) * e.height / bitmap.getHeight());
        cx = (e.width - cw) / 2;
      }
    }
    if (cw <= 0 || ch <= 0) return;
    if (static_cast<CoverFilter>(e.coverFilter) == CoverFilter::Grey && (e.flags & FLAG_TRANSFORMED) == 0 &&
        grey != nullptr && grey->count < GreyPlan::MAX_IMAGES) {
      return paintGreyCover(e, bitmap, path, isSleepImage);
    }
    const GfxRenderer::ResampleFilter filter = resampleFilterOf(e);

    if (e.flags & FLAG_TRANSFORMED) {
      int rowBytes = 0;
      auto scratch = allocScratch(cw, ch, rowBytes);
      if (scratch && GfxRenderer::resampleBitmapTo1Bit(bitmap, cw, ch, scratch.get(), rowBytes, filter, source)) {
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
    renderer.drawBitmapResampled(bitmap, x, y, cw, ch, filter, source);
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

  // Grey covers: placed like drawBitmap places them (it only shrinks), so the
  // grey passes can redraw them exactly. contain fits the whole image; zoom
  // crops it to the box's shape first. drawBitmap cannot stretch, so fill is
  // drawn as zoom too.
  void paintGreyCover(const Element& e, const Bitmap& bitmap, const std::string& path, const bool dither) const {
    const float srcW = static_cast<float>(bitmap.getWidth());
    const float srcH = static_cast<float>(bitmap.getHeight());
    float cropX = 0, cropY = 0;
    if ((e.flags & FLAG_CONTAIN) == 0) {
      const float boxAspect = static_cast<float>(e.width) / static_cast<float>(e.height);
      const float imgAspect = srcW / srcH;
      if (imgAspect > boxAspect) {
        cropX = 1.0f - boxAspect / imgAspect;
      } else {
        cropY = 1.0f - imgAspect / boxAspect;
      }
    }
    const float croppedW = (1.0f - cropX) * srcW;
    const float croppedH = (1.0f - cropY) * srcH;
    const float scale = std::min(1.0f, std::min(e.width / croppedW, e.height / croppedH));
    const int drawnW = std::max(1, static_cast<int>(croppedW * scale));
    const int drawnH = std::max(1, static_cast<int>(croppedH * scale));
    GreyImage& g = grey->images[grey->count++];
    g.path = path;
    g.x = static_cast<int16_t>(roundFp(e.matrix[4]) + (e.width - drawnW) / 2);
    g.y = static_cast<int16_t>(roundFp(e.matrix[5]) + (e.height - drawnH) / 2);
    g.width = static_cast<int16_t>(drawnW);
    g.height = static_cast<int16_t>(drawnH);
    g.cropX = cropX;
    g.cropY = cropY;
    g.dither = dither;
    renderer.fillRect(roundFp(e.matrix[4]), roundFp(e.matrix[5]), e.width, e.height, false);
    renderer.drawBitmap(bitmap, g.x, g.y, g.width, g.height, g.cropX, g.cropY);
  }

  GfxRenderer& renderer;
  const ThemeFonts& fonts;
  const char* strings;
  uint16_t stringsSize;
  const std::string& coverPath;
  const std::string& sleepImagePath;
  GreyPlan* grey;
};

bool readExact(FsFile& file, void* dst, const size_t n) { return file.read(dst, n) == static_cast<int>(n); }

// bg.bmp is a screen-sized 1-bit image, so its rows can go straight into the
// framebuffer as 1-bit "glyphs" (drawGlyphBitmap resolves the orientation once
// per row), instead of through drawBitmap's scaler a pixel at a time. Over a
// cleared (white) screen only the black pixels are written. False when the
// file can't be read this way; the caller falls back to drawBitmap.
bool blitBackground(const GfxRenderer& renderer, FsFile& file, const Bitmap& bitmap) {
  // Which palette entry is black: BITMAPINFOHEADER size at 14, palette after it.
  uint8_t raw[4];
  if (!file.seekSet(14) || file.read(raw, 4) != 4) return false;
  uint32_t infoSize;
  memcpy(&infoSize, raw, sizeof(infoSize));
  uint8_t palette[8];
  if (!file.seekSet(14 + infoSize) || file.read(palette, sizeof(palette)) != static_cast<int>(sizeof(palette))) {
    return false;
  }
  // BGRA; a dark entry is ink.
  const auto luminance = [](const uint8_t* bgra) { return (bgra[0] * 29 + bgra[1] * 150 + bgra[2] * 77) >> 8; };
  const bool zeroIsBlack = luminance(palette) < luminance(palette + 4);
  if ((luminance(palette) < 128) == (luminance(palette + 4) < 128)) return false;  // not black and white
  if (bitmap.rewindToData() != BmpReaderError::Ok) return false;

  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  const int rowBytes = bitmap.getRowBytes();
  constexpr int BAND_ROWS = 16;
  // Heap: a band of rows (~1 KB for 480 px) for this draw only, read in one
  // go so the card isn't asked for 60 bytes at a time.
  auto band = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(rowBytes) * BAND_ROWS);
  if (!band) {
    LOG_ERR("STH", "malloc failed: %d bytes background band", rowBytes * BAND_ROWS);
    return false;
  }
  for (int first = 0; first < height; first += BAND_ROWS) {
    const int rows = std::min(BAND_ROWS, height - first);
    const int bytes = rowBytes * rows;
    if (file.read(band.get(), static_cast<size_t>(bytes)) != bytes) {
      LOG_ERR("STH", "Background truncated at row %d", first);
      return true;  // part drawn; the rest stays white rather than drawn twice
    }
    for (int r = 0; r < rows; r++) {
      uint8_t* row = band.get() + static_cast<size_t>(r) * rowBytes;
      // drawGlyphBitmap inks set bits.
      if (zeroIsBlack) {
        for (int i = 0; i < rowBytes; i++) row[i] = static_cast<uint8_t>(~row[i]);
      }
      const int fileRow = first + r;
      const int y = bitmap.isTopDown() ? fileRow : height - 1 - fileRow;
      renderer.drawGlyphBitmap(row, width, 1, glyphBitmap::Frame{0, y, 1, 0, 0, 1}, /*twoBit=*/false, GfxRenderer::BW,
                               /*state=*/true);
    }
  }
  return true;
}

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
  if (!blitBackground(renderer, file, bitmap)) {
    // Slow but general: the scaler, a pixel at a time (~260 ms for a screen).
    bitmap.rewindToData();
    renderer.drawBitmap(bitmap, 0, 0, header.screenWidth, header.screenHeight);
  }
  return true;
}

// bg.bmp's pixels where maskName (a 1-bit BMP of the same size) is black;
// everything else is left as it is. An overlay's background goes through
// mask.bmp over the screen that was showing; art in front of images goes
// through front.bmp after them. greyPass: in a grey render mode, mark the
// masked pixels "leave as the BW base drew them" instead.
bool drawThroughMask(const GfxRenderer& renderer, const char* dir, const int w, const int h, const char* maskName,
                     const bool greyPass = false) {
  char bgPath[PATH_LEN];
  char maskPath[PATH_LEN];
  snprintf(bgPath, sizeof(bgPath), "%s/bg.bmp", dir);
  snprintf(maskPath, sizeof(maskPath), "%s/%s", dir, maskName);
  FsFile bgFile;
  FsFile maskFile;
  if (!Storage.openFileForRead("STH", bgPath, bgFile) || !Storage.openFileForRead("STH", maskPath, maskFile)) {
    LOG_ERR("STH", "Theme needs %s and %s", bgPath, maskPath);
    return false;
  }
  Bitmap bg(bgFile);
  Bitmap mask(maskFile);
  const auto usable = [w, h](Bitmap& b) {
    return b.parseHeaders() == BmpReaderError::Ok && b.is1Bit() && b.getWidth() == w && b.getHeight() == h;
  };
  if (!usable(bg) || !usable(mask)) {
    LOG_ERR("STH", "bg.bmp and %s in %s must be %dx%d 1-bit BMPs", maskName, dir, w, h);
    return false;
  }
  // Heap: two decoded rows (2 bits per pixel) and two raw rows, ~600 bytes for
  // an 800px row: over the stack budget, and freed when the draw returns.
  const int outRow = (w + 3) / 4;
  const size_t bytes = static_cast<size_t>(outRow) * 2 + static_cast<size_t>(bg.getRowBytes() + mask.getRowBytes());
  auto rows = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!rows) {
    LOG_ERR("STH", "malloc failed: %u bytes mask rows", static_cast<unsigned>(bytes));
    return false;
  }
  uint8_t* bgOut = rows.get();
  uint8_t* maskOut = bgOut + outRow;
  uint8_t* bgRaw = maskOut + outRow;
  uint8_t* maskRaw = bgRaw + bg.getRowBytes();
  for (int row = 0; row < h; row++) {
    if (bg.readNextRow(bgOut, bgRaw) != BmpReaderError::Ok ||
        mask.readNextRow(maskOut, maskRaw) != BmpReaderError::Ok) {
      LOG_ERR("STH", "bg.bmp or %s in %s is truncated", maskName, dir);
      return false;
    }
    const int bgY = bg.isTopDown() ? row : h - 1 - row;
    const int maskY = mask.isTopDown() ? row : h - 1 - row;
    if (bgY != maskY) {
      LOG_ERR("STH", "bg.bmp and %s in %s differ in row order", maskName, dir);
      return false;
    }
    for (int x = 0; x < w; x++) {
      const int shift = 6 - (x % 4) * 2;
      if (((maskOut[x / 4] >> shift) & 0x3) != 0) continue;  // white in the mask: keep the screen
      renderer.drawPixel(x, bgY, greyPass || ((bgOut[x / 4] >> shift) & 0x3) == 0);
    }
  }
  return true;
}

// Opens and validates theme.bin and reads its layout (elements, then the
// string table, NUL-terminated). dir receives the theme folder.
// The store's "cover-crop" theme is one full-screen image over the built-in
// logo screen, so the firmware can lay it out itself when its folder isn't on
// the card: the same element the compiler writes for it, the open book's cover
// filling the screen with its overflow cropped (object-fit: cover).
struct ImageBuiltIn {
  const char* name;
  const char* tpl;  // the image value
};
constexpr ImageBuiltIn IMAGE_BUILT_INS[] = {
    {"cover-crop", "{{book.cover}}"},
};

const ImageBuiltIn* imageBuiltIn(const char* themeName) {
  for (const auto& b : IMAGE_BUILT_INS) {
    if (strcmp(themeName, b.name) == 0) return &b;
  }
  return nullptr;
}

// Lays out an image built-in at the renderer's screen (zero size without one:
// inspect() doesn't need it). The background is drawBuiltIn()'s logo screen.
bool synthesizeLayout(const ImageBuiltIn& b, const GfxRenderer* renderer, Header& header,
                      std::unique_ptr<uint8_t[]>& layout) {
  const size_t tplLen = strlen(b.tpl) + 1;
  // Heap, like a loaded layout: one element and its template, for this draw.
  layout = makeUniqueNoThrow<uint8_t[]>(ELEMENT_SIZE + tplLen + 1);
  if (!layout) {
    LOG_ERR("STH", "malloc failed: built-in theme layout");
    return false;
  }
  const uint16_t w = renderer ? static_cast<uint16_t>(renderer->getScreenWidth()) : 0;
  const uint16_t h = renderer ? static_cast<uint16_t>(renderer->getScreenHeight()) : 0;
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, MAGIC, sizeof(MAGIC));
  header.version = VERSION;
  header.screenWidth = w;
  header.screenHeight = h;
  header.orientation = renderer ? static_cast<uint8_t>(renderer->getOrientation()) : 0;
  header.elementCount = 1;
  header.flags = HEADER_FLAG_COVER_TEMPLATES;
  header.stringTableSize = static_cast<uint16_t>(tplLen);

  Element e{};
  e.kind = static_cast<uint8_t>(ElementKind::Cover);
  e.flags = FLAG_OPAQUE | FLAG_ZOOM;
  e.digitIndex = -1;
  e.width = w;
  e.height = h;
  e.templateOffset = 0;
  e.matrix[0] = FP_ONE;
  e.matrix[3] = FP_ONE;
  e.coverFilter = static_cast<uint8_t>(CoverFilter::Grey);
  memcpy(layout.get(), &e, ELEMENT_SIZE);
  memcpy(layout.get() + ELEMENT_SIZE, b.tpl, tplLen);
  layout[ELEMENT_SIZE + tplLen] = '\0';
  return true;
}

// synthetic: set when the theme is an image built-in laid out in memory (its
// folder isn't on the card); the caller draws drawBuiltIn() as its background.
bool loadLayout(const char* themeName, char (&dir)[PATH_LEN], Header& header, std::unique_ptr<uint8_t[]>& layout,
                const GfxRenderer* renderer, bool& synthetic) {
  synthetic = false;
  if (themeName == nullptr || themeName[0] == '\0' || strchr(themeName, '/') != nullptr) {
    LOG_ERR("STH", "No usable theme name set");
    return false;
  }
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, themeName);
  char path[PATH_LEN];
  snprintf(path, sizeof(path), "%s/theme.bin", dir);

  FsFile file;
  if (!Storage.exists(path)) {
    if (const ImageBuiltIn* b = imageBuiltIn(themeName)) {
      synthetic = synthesizeLayout(*b, renderer, header, layout);
      return synthetic;
    }
  }
  if (!Storage.openFileForRead("STH", path, file)) {
    LOG_ERR("STH", "Missing %s", path);
    return false;
  }
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

  // Heap: the layout (<= 64 x 48 B elements + <= 4KB strings) for this draw
  // only. Static storage would hold ~7KB of DRAM for the whole uptime.
  const size_t elementBytes = static_cast<size_t>(header.elementCount) * ELEMENT_SIZE;
  layout = makeUniqueNoThrow<uint8_t[]>(elementBytes + header.stringTableSize + 1);
  if (!layout) {
    LOG_ERR("STH", "malloc failed: %u bytes theme layout",
            static_cast<unsigned>(elementBytes + header.stringTableSize));
    return false;
  }
  if (!readExact(file, layout.get(), elementBytes + header.stringTableSize)) {
    LOG_ERR("STH", "%s is truncated", path);
    return false;
  }
  layout[elementBytes + header.stringTableSize] = '\0';
  return true;
}

Element elementAt(const std::unique_ptr<uint8_t[]>& layout, const uint8_t i) {
  Element e;
  memcpy(&e, layout.get() + static_cast<size_t>(i) * ELEMENT_SIZE, sizeof(e));
  return e;
}

}  // namespace

bool inspect(const char* themeName, ThemeInfo& out) {
  out = ThemeInfo{};
  char dir[PATH_LEN];
  Header header;
  std::unique_ptr<uint8_t[]> layout;
  bool synthetic;
  if (!loadLayout(themeName, dir, header, layout, nullptr, synthetic)) return false;
  out.overlay = (header.flags & HEADER_FLAG_OVERLAY) != 0;
  out.width = header.screenWidth;
  out.height = header.screenHeight;
  const char* strings =
      reinterpret_cast<const char*>(layout.get() + static_cast<size_t>(header.elementCount) * ELEMENT_SIZE);
  for (uint8_t i = 0; i < header.elementCount; i++) {
    const Element e = elementAt(layout, i);
    if (static_cast<ElementKind>(e.kind) != ElementKind::Cover) continue;
    // An overlay keeps the screen that is showing, which the grey passes'
    // scrub would wipe: its grey images are dithered instead.
    if (!out.overlay && static_cast<CoverFilter>(e.coverFilter) == CoverFilter::Grey &&
        (e.flags & FLAG_TRANSFORMED) == 0) {
      out.greyscale = true;
    }
    SleepThemeVars::Id id;
    const char* tpl = e.templateOffset < header.stringTableSize ? strings + e.templateOffset : "";
    if (coverVar(tpl, id) && id == SleepThemeVars::Id::SleepImage) out.showsSleepImage = true;
  }
  return true;
}

bool draw(GfxRenderer& renderer, const char* themeName, GreyPlan* grey) {
  char dir[PATH_LEN];
  Header header;
  std::unique_ptr<uint8_t[]> layout;
  bool synthetic;
  if (!loadLayout(themeName, dir, header, layout, &renderer, synthetic)) return false;
  const size_t elementBytes = static_cast<size_t>(header.elementCount) * ELEMENT_SIZE;
  const char* strings = reinterpret_cast<const char*>(layout.get() + elementBytes);
  const bool overlay = (header.flags & HEADER_FLAG_OVERLAY) != 0;
  if (overlay) grey = nullptr;
  if (grey) {
    grey->count = 0;
    grey->frontDir.clear();
  }

  renderer.setOrientation(static_cast<GfxRenderer::Orientation>(header.orientation));
  if (renderer.getScreenWidth() != header.screenWidth || renderer.getScreenHeight() != header.screenHeight) {
    LOG_ERR("STH", "Theme laid out for %ux%u, screen is %dx%d", header.screenWidth, header.screenHeight,
            renderer.getScreenWidth(), renderer.getScreenHeight());
    return false;
  }

  LOG_DBG("STH", "Theme %s: %u elements, %u fonts%s; heap free=%u largest=%u", themeName, header.elementCount,
          header.fontCount, overlay ? ", overlay" : "", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Images named once per draw. The cover lookup may borrow the framebuffer,
  // so it runs before any paint; the sleep image is a random pick, made once
  // so every element showing it shows the same one.
  std::string coverPath;
  std::string sleepImagePath;
  bool coverDone = false;
  bool sleepImageDone = false;
  for (uint8_t i = 0; i < header.elementCount; i++) {
    const Element e = elementAt(layout, i);
    if (static_cast<ElementKind>(e.kind) != ElementKind::Cover) continue;
    if ((header.flags & HEADER_FLAG_COVER_TEMPLATES) == 0) {
      LOG_INF("STH",
              "Theme %s was compiled before covers named their book: every cover shows the open book. "
              "Recompile it on the Sleep Themes page.",
              themeName);
    }
    // A cover with no template is book.cover: themes compiled before
    // recent.N.cover existed stored none.
    SleepThemeVars::Id id;
    const char* tpl = e.templateOffset < header.stringTableSize ? strings + e.templateOffset : "";
    const bool named = coverVar(tpl, id);
    if (named && id == SleepThemeVars::Id::SleepImage) {
      if (!sleepImageDone && !SleepImage::pickPath(sleepImagePath)) sleepImagePath.clear();
      sleepImageDone = true;
    } else if ((!named || id == SleepThemeVars::Id::BookCover) && !coverDone) {
      if (!SleepCover::resolveBmpPath(renderer, (e.flags & FLAG_CONTAIN) == 0, coverPath)) coverPath.clear();
      coverDone = true;
    }
  }

  if (synthetic) {
    drawBuiltIn(renderer, false);
  } else if (!(overlay ? drawThroughMask(renderer, dir, header.screenWidth, header.screenHeight, "mask.bmp")
                       : drawBackground(renderer, dir, header))) {
    return false;
  }

  ThemeFonts fonts(renderer);
  fonts.load(dir, header.fontCount);
  SleepThemeVars::beginFrame();

  // Letter spacing the CSS way while the theme draws, back to the reader's after.
  const TrackSpacesScope trackSpaces(renderer);
  // Images first, then any static art that sits in front of them, then live
  // text and bars, which are always in front.
  const Painter painter(renderer, fonts, strings, header.stringTableSize, coverPath, sleepImagePath, grey);
  for (uint8_t i = 0; i < header.elementCount; i++) {
    const Element e = elementAt(layout, i);
    if (static_cast<ElementKind>(e.kind) == ElementKind::Cover) painter.paint(e);
  }
  FlowCursor flow;
  if (header.flags & HEADER_FLAG_FRONT) {
    if (drawThroughMask(renderer, dir, header.screenWidth, header.screenHeight, "front.bmp") && grey) {
      grey->frontDir = dir;
    }
  }
  for (uint8_t i = 0; i < header.elementCount; i++) {
    const Element e = elementAt(layout, i);
    if (static_cast<ElementKind>(e.kind) == ElementKind::Box) {
      // Drawn before (under) the text it fits; that text is the next element.
      if (i + 1 >= header.elementCount) continue;
      const Element text = elementAt(layout, i + 1);
      if (e.flags & FLAG_FLOW) {
        painter.paintFlowItem(e, text, flow);
        i++;  // drawn with its box
      } else {
        painter.paintBox(e, text);
      }
      continue;
    }
    if (static_cast<ElementKind>(e.kind) != ElementKind::Cover) painter.paint(e);
  }
  SleepThemeVars::endFrame();

  LOG_DBG("STH", "Theme drawn; heap free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

void drawBuiltIn(GfxRenderer& renderer, const bool dark) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));
  if (dark) renderer.invertScreen();
}

void showSleepImages() {
  ThemeInfo info;
  if (inspect(SETTINGS.sleepTheme, info) && info.showsSleepImage) return;
  constexpr char CUSTOM_THEME[] = "custom";
  if (strcmp(SETTINGS.sleepTheme, CUSTOM_THEME) == 0) return;  // chosen, just not installed yet
  strncpy(SETTINGS.sleepTheme, CUSTOM_THEME, sizeof(SETTINGS.sleepTheme) - 1);
  SETTINGS.sleepTheme[sizeof(SETTINGS.sleepTheme) - 1] = '\0';
  SETTINGS.saveToFile();
}

void drawGreyImages(const GfxRenderer& renderer, const GreyPlan& plan) {
  for (uint8_t i = 0; i < plan.count; i++) {
    const GreyImage& g = plan.images[i];
    FsFile file;
    if (!Storage.openFileForRead("STH", g.path, file)) continue;
    Bitmap bitmap(file, g.dither);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) continue;
    renderer.drawBitmap(bitmap, g.x, g.y, g.width, g.height, g.cropX, g.cropY);
  }
  // Art in front of the images stays as the BW base drew it.
  if (!plan.frontDir.empty()) {
    drawThroughMask(renderer, plan.frontDir.c_str(), renderer.getScreenWidth(), renderer.getScreenHeight(), "front.bmp",
                    /*greyPass=*/true);
  }
}

}  // namespace SleepThemeRenderer
