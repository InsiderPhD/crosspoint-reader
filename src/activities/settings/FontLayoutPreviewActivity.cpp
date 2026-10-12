#include "FontLayoutPreviewActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "BookSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

// ---- Row table -------------------------------------------------------------
// Enum value labels reuse the StrIds already defined for the flat Reader
// settings list (SettingsList.h) so translations stay in one place.
namespace {
constexpr StrId kFamilyLabels[] = {StrId::STR_BOOKERLY, StrId::STR_INTER, StrId::STR_OPEN_DYSLEXIC,
                                   StrId::STR_MONOSPACE};
constexpr StrId kSizeLabels[] = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
constexpr StrId kSpacingLabels[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE};
// Same order as the persisted index (SettingsList.h): -1.5 px .. +1.5 px.
constexpr StrId kCharSpacingLabels[] = {
    StrId::STR_SPACING_MINUS_3, StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
    StrId::STR_SPACING_PLUS_1,  StrId::STR_SPACING_PLUS_2,  StrId::STR_SPACING_PLUS_3};
constexpr StrId kAlignLabels[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                  StrId::STR_BOOK_S_STYLE};
// Same order as the persisted index (TextLayoutOptions::PARAGRAPH_INDENT_*).
constexpr StrId kIndentLabels[] = {StrId::STR_BOOK_S_STYLE, StrId::STR_NONE_OPT, StrId::STR_SMALL,
                                   StrId::STR_MEDIUM,       StrId::STR_LARGE,    StrId::STR_X_LARGE};
constexpr StrId kImageLabels[] = {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS};
constexpr StrId kOrientLabels[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                   StrId::STR_LANDSCAPE_CCW};

// Sample paragraphs shown in the preview (shared by the renderer and the SD
// glyph prewarm so the same characters are cached).
constexpr StrId kSampleParas[] = {StrId::STR_PREVIEW_SAMPLE_1, StrId::STR_PREVIEW_SAMPLE_2,
                                  StrId::STR_PREVIEW_SAMPLE_3};

// Longest word we measure/draw in the preview; sample text is plain ASCII.
constexpr int kWordBufSize = 96;
constexpr int kMaxWordsPerParagraph = 64;

// Bionic bold-prefix length in bytes, matching the real engine
// (ParsedText.cpp: clamp(ceil(43% of letters), 1, 9)). Sample text is ASCII so
// byte count == codepoint count.
int boldPrefixLen(int len) {
  int n = (len * 43 + 99) / 100;
  if (n < 1) n = 1;
  if (n > 9) n = 9;
  if (n > len) n = len;
  return n;
}

struct PreviewWord {
  const char* s;
  int len;
  int width;
};

// Width (advance) of a single word, honoring the bionic bold prefix. `tracking` is the
// reader's letter spacing (half-pixel steps), applied as the real engine applies it: inside
// each run, plus one boundary where the bold prefix meets its regular suffix.
int measureWord(const GfxRenderer& renderer, int fontId, const char* s, int len, bool bionic, int8_t tracking) {
  char buf[kWordBufSize];
  if (len >= kWordBufSize) len = kWordBufSize - 1;
  if (!bionic) {
    snprintf(buf, sizeof(buf), "%.*s", len, s);
    return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, tracking);
  }
  const int boldLen = boldPrefixLen(len);
  snprintf(buf, sizeof(buf), "%.*s", boldLen, s);
  int w = renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::BOLD, tracking);
  if (boldLen < len) {
    w += GfxRenderer::trackingPixels(1, tracking);
    snprintf(buf, sizeof(buf), "%.*s", len - boldLen, s + boldLen);
    w += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, tracking);
  }
  return w;
}

// Draw one word at (x, y-top); returns its advance so callers can position the
// next word. Splits into bold prefix + regular suffix when bionic is on.
int drawWord(const GfxRenderer& renderer, int fontId, const char* s, int len, bool bionic, int x, int y,
             int8_t tracking) {
  char buf[kWordBufSize];
  if (len >= kWordBufSize) len = kWordBufSize - 1;
  if (!bionic) {
    snprintf(buf, sizeof(buf), "%.*s", len, s);
    renderer.drawText(fontId, x, y, buf, true, EpdFontFamily::REGULAR, tracking);
    return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, tracking);
  }
  const int boldLen = boldPrefixLen(len);
  snprintf(buf, sizeof(buf), "%.*s", boldLen, s);
  renderer.drawText(fontId, x, y, buf, true, EpdFontFamily::BOLD, tracking);
  int w = renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::BOLD, tracking);
  if (boldLen < len) {
    w += GfxRenderer::trackingPixels(1, tracking);
    snprintf(buf, sizeof(buf), "%.*s", len - boldLen, s + boldLen);
    renderer.drawText(fontId, x + w, y, buf, true, EpdFontFamily::REGULAR, tracking);
    w += renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR, tracking);
  }
  return w;
}

// Draw a packed line of words with the given alignment. `naturalWidth` is the
// line width at single spacing; `lineMax` is the available width.
void drawLine(const GfxRenderer& renderer, int fontId, const PreviewWord* words, int count, int naturalWidth,
              int lineMax, int lineLeft, int y, uint8_t align, bool bionic, int spaceW, bool isLastLine,
              int8_t tracking) {
  const bool justify =
      (align == CrossPointSettings::JUSTIFIED || align == CrossPointSettings::BOOK_STYLE) && !isLastLine && count > 1;

  if (justify) {
    int extra = lineMax - naturalWidth;
    if (extra < 0) extra = 0;
    const int gaps = count - 1;
    const int base = spaceW + extra / gaps;
    const int rem = extra % gaps;
    int x = lineLeft;
    for (int k = 0; k < count; ++k) {
      x += drawWord(renderer, fontId, words[k].s, words[k].len, bionic, x, y, tracking);
      if (k < count - 1) x += base + (k < rem ? 1 : 0);
    }
    return;
  }

  int x = lineLeft;
  if (align == CrossPointSettings::CENTER_ALIGN) {
    x = lineLeft + (lineMax - naturalWidth) / 2;
  } else if (align == CrossPointSettings::RIGHT_ALIGN) {
    x = lineLeft + (lineMax - naturalWidth);
  }
  for (int k = 0; k < count; ++k) {
    x += drawWord(renderer, fontId, words[k].s, words[k].len, bionic, x, y, tracking);
    if (k < count - 1) x += spaceW;
  }
}

// Lay out one paragraph via greedy word-wrap; returns the y below the last line
// drawn. Stops early once the next line would exceed `regionBottom`.
int renderParagraph(const GfxRenderer& renderer, const char* text, int fontId, int lineH, int textLeft, int maxWidth,
                    uint8_t align, bool bionic, int spaceW, int indentPx, int y, int regionBottom, int8_t tracking) {
  PreviewWord words[kMaxWordsPerParagraph];
  int nWords = 0;
  const char* p = text;
  while (*p && nWords < kMaxWordsPerParagraph) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ') ++p;
    words[nWords].s = start;
    words[nWords].len = static_cast<int>(p - start);
    words[nWords].width = measureWord(renderer, fontId, start, words[nWords].len, bionic, tracking);
    ++nWords;
  }

  int i = 0;
  bool firstLine = true;
  while (i < nWords && y + lineH <= regionBottom) {
    const int lineMax = maxWidth - (firstLine ? indentPx : 0);
    int j = i;
    int naturalWidth = words[i].width;
    int count = 1;
    while (j + 1 < nWords) {
      const int add = spaceW + words[j + 1].width;
      if (naturalWidth + add > lineMax && count > 0) break;
      naturalWidth += add;
      ++j;
      ++count;
    }
    const bool isLastLine = (j + 1 >= nWords);
    const int lineLeft = textLeft + (firstLine ? indentPx : 0);
    drawLine(renderer, fontId, words + i, count, naturalWidth, lineMax, lineLeft, y, align, bionic, spaceW, isLastLine,
             tracking);
    y += lineH;
    i = j + 1;
    firstLine = false;
  }
  return y;
}
}  // namespace

// Table order = display order in the list.
const FontLayoutPreviewActivity::Row FontLayoutPreviewActivity::kRows[] = {
    {StrId::STR_FONT_FAMILY, RowKind::Enum, &CrossPointSettings::fontFamily, kFamilyLabels,
     CrossPointSettings::FONT_FAMILY_COUNT, 0, 0, 0, true, false},
    {StrId::STR_FONT_SIZE, RowKind::Enum, &CrossPointSettings::fontSize, kSizeLabels,
     CrossPointSettings::FONT_SIZE_COUNT, 0, 0, 0, true, false},
    {StrId::STR_LINE_SPACING, RowKind::Enum, &CrossPointSettings::lineSpacing, kSpacingLabels,
     CrossPointSettings::LINE_COMPRESSION_COUNT, 0, 0, 0, false, false},
    {StrId::STR_WORD_SPACING, RowKind::Value, &CrossPointSettings::wordSpacing, nullptr, 0,
     CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX, CrossPointSettings::WORD_SPACING_STEP,
     false, false},
    {StrId::STR_CHARACTER_SPACING, RowKind::Enum, &CrossPointSettings::characterSpacing, kCharSpacingLabels,
     CrossPointSettings::CHARACTER_SPACING_COUNT, 0, 0, 0, false, false},
    {StrId::STR_PARA_ALIGNMENT, RowKind::Enum, &CrossPointSettings::paragraphAlignment, kAlignLabels,
     CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, 0, 0, 0, false, false},
    {StrId::STR_SCREEN_MARGIN, RowKind::Value, &CrossPointSettings::screenMargin, nullptr, 0, 5, 40, 5, false, false},
    {StrId::STR_EXTRA_SPACING, RowKind::Toggle, &CrossPointSettings::extraParagraphSpacing, nullptr, 0, 0, 0, 0, false,
     false},
    {StrId::STR_PARAGRAPH_INDENT, RowKind::Enum, &CrossPointSettings::paragraphIndent, kIndentLabels,
     TextLayoutOptions::PARAGRAPH_INDENT_COUNT, 0, 0, 0, false, false},
    {StrId::STR_BIONIC_READING, RowKind::Toggle, &CrossPointSettings::bionicReading, nullptr, 0, 0, 0, 0, false, false},
    {StrId::STR_TEXT_AA, RowKind::Toggle, &CrossPointSettings::textAntiAliasing, nullptr, 0, 0, 0, 0, false, false},
    {StrId::STR_READER_DARK_MODE, RowKind::Toggle, &CrossPointSettings::darkMode, nullptr, 0, 0, 0, 0, false, false},
    {StrId::STR_ORIENTATION, RowKind::Enum, &CrossPointSettings::orientation, kOrientLabels,
     CrossPointSettings::ORIENTATION_COUNT, 0, 0, 0, false, true},
    // Font Download deliberately NOT here: launching it from inside this
    // activity keeps the whole live preview (prewarmed sample glyphs, preview
    // state) resident under the download's TLS session — observed on-device
    // eating the transfer's heap. It lives in the main Settings list instead.

    // Book Settings only (see rowInMode): reader options that live in the flat
    // Settings list globally, offered here so one book can differ.
    {StrId::STR_HYPHENATION, RowKind::Toggle, &CrossPointSettings::hyphenationEnabled, nullptr, 0, 0, 0, 0, false,
     false},
    {StrId::STR_IMAGES, RowKind::Enum, &CrossPointSettings::imageRendering, kImageLabels,
     CrossPointSettings::IMAGE_RENDERING_COUNT, 0, 0, 0, false, false},
    // Last, so the cursor never starts on it.
    {StrId::STR_USE_GLOBAL_SETTINGS, RowKind::Action, nullptr, nullptr, 0, 0, 0, 0, false, false},
};
const int FontLayoutPreviewActivity::kRowCount = sizeof(kRows) / sizeof(kRows[0]);

bool FontLayoutPreviewActivity::rowInMode(const Row& row, const Mode mode) {
  // Device-wide by design: both are flipped from the reader menu and by reader
  // actions, and a per-book copy would fight those.
  const bool globalOnly = row.field == &CrossPointSettings::darkMode || row.field == &CrossPointSettings::orientation;
  const bool bookOnly = row.kind == RowKind::Action || row.field == &CrossPointSettings::hyphenationEnabled ||
                        row.field == &CrossPointSettings::imageRendering;
  return mode == Mode::Book ? !globalOnly : !bookOnly;
}

FontLayoutPreviewActivity::FontLayoutPreviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const Mode mode)
    : Activity("FontLayoutPreview", renderer, mappedInput), mode(mode) {
  static_assert(sizeof(kRows) / sizeof(kRows[0]) <= kMaxRows, "grow kMaxRows");
  for (int i = 0; i < kRowCount; ++i) {
    if (rowInMode(kRows[i], mode)) rowMap[rowCount++] = static_cast<uint8_t>(i);
  }
}

void FontLayoutPreviewActivity::onEnter() {
  Activity::onEnter();
  // An active SD font needs a file + glyph load before the first paint — show a
  // popup so the initial open isn't a frozen-looking screen. Built-ins are instant.
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    GUI.drawPopup(renderer, tr(STR_LOADING_FONT));
  }
  // Make sure whatever font is currently active (built-in or SD) is ready to draw.
  ensureSdFontLoaded();
  prewarmSampleGlyphs();
  entryOrientation = renderer.getOrientation();
  requestUpdate();
}

void FontLayoutPreviewActivity::onExit() {
  Activity::onExit();
  if (changed) {
    // A book's settings go to its sidecar; settings.json is not touched.
    if (mode == Mode::Book) {
      BookSettings::save();
    } else {
      SETTINGS.saveToFile();
    }
  }
  free(previewSnap);
  previewSnap = nullptr;
  previewSnapCap = 0;
  // Leave the renderer as we found it; the caller re-applies whatever
  // orientation it needs (the reader re-applies SETTINGS.orientation, the UI
  // stays Portrait). Persisted SETTINGS.orientation survives regardless.
  renderer.setOrientation(entryOrientation);
}

void FontLayoutPreviewActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Full Touch (X4 Pro): tap a row to select it, tap it again to change it.
  // Taps in the preview half fall on dead space and are dropped.
  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), rowCount, selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      activateSelectedRow();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelectedRow();
    return;
  }

  // Full Touch: a vertical swipe turns a whole page of rows, matching the held
  // side key. Inert while every row fits on one page.
  const int pageItems = GUI.listGeometry(listRect(), selectedIndex, /*hasSubtitle=*/false).pageItems;
  int swipeIndex = selectedIndex;
  if (TouchListNav::pageSwipe(mappedInput, rowCount, pageItems, swipeIndex)) {
    selectedIndex = swipeIndex;
    requestUpdate();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rowCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rowCount);
    requestUpdate();
  });
}

// Shared by the Confirm button and the Full Touch second tap: Action rows open
// their sub-activity, everything else cycles its value in place.
void FontLayoutPreviewActivity::activateSelectedRow() {
  const Row& row = rowAt(selectedIndex);
  if (row.kind == RowKind::Action) {
    activateRow(row);
    return;
  }
  changeRow(row);
  requestUpdate();
}

void FontLayoutPreviewActivity::changeRow(const Row& row) {
  changed = true;
  previewDirty = true;  // any setting change re-renders the sample region

  // Book Settings: the row becomes this book's override. Must come before the
  // value moves, while SETTINGS still holds the global one to remember.
  if (mode == Mode::Book) BookSettings::beginOverride(row.field);

  // Font family has its own cycle (built-ins + installed SD fonts).
  if (row.field == &CrossPointSettings::fontFamily) {
    cycleFont();
    return;
  }

  switch (row.kind) {
    case RowKind::Enum: {
      const uint8_t count = row.enumCount;
      if (count == 0) break;
      const uint8_t v = SETTINGS.*(row.field);
      SETTINGS.*(row.field) = static_cast<uint8_t>((v + 1) % count);
      break;
    }
    case RowKind::Toggle:
      SETTINGS.*(row.field) = !(SETTINGS.*(row.field));
      break;
    case RowKind::Value: {
      int v = SETTINGS.*(row.field);
      v += row.valStep;
      if (v > row.valMax) v = row.valMin;
      SETTINGS.*(row.field) = static_cast<uint8_t>(v);
      break;
    }
    case RowKind::Action:
      return;
  }

  if (row.affectsFont) {
    // Reloading an SD font at a new size reads a file from the card — show a
    // brief popup so the wait isn't a frozen screen. Built-ins are instant.
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      GUI.drawPopup(renderer, tr(STR_LOADING_FONT));
    }
    ensureSdFontLoaded();   // font size may need a different SD glyph file
    prewarmSampleGlyphs();  // re-cache sample glyph bitmaps at the new size
  }
  // Anti-aliasing is a grayscale double-render (a hard refresh), so only run it
  // once — right after the user enables AA — instead of on every keystroke.
  if (row.field == &CrossPointSettings::textAntiAliasing && SETTINGS.textAntiAliasing) {
    aaPreviewNext = true;
  }
  // Dark mode flips every pixel's polarity; a differential fast refresh would ghost badly.
  if (row.field == &CrossPointSettings::darkMode) {
    fullRefreshNext = true;
    // Leaving dark mode makes the AA pass (skipped while dark) visible again.
    if (!SETTINGS.darkMode && SETTINGS.textAntiAliasing) {
      aaPreviewNext = true;
    }
  }
  if (row.isOrientation) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    fullRefreshNext = true;  // orientation flips must full-refresh to avoid ghosting
  }
}

void FontLayoutPreviewActivity::cycleFont() {
  const auto& families = sdFontSystem.registry().getFamilies();
  const int sdCount = static_cast<int>(families.size());
  const int total = CrossPointSettings::BUILTIN_FONT_COUNT + sdCount;

  // Resolve the current position in the combined [built-ins..SD] list.
  int cur = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    for (int i = 0; i < sdCount; ++i) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        cur = CrossPointSettings::BUILTIN_FONT_COUNT + i;
        break;
      }
    }
  }

  const int next = (cur + 1) % total;
  if (next < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = static_cast<uint8_t>(next);
    SETTINGS.sdFontFamilyName[0] = '\0';  // built-in: clear the SD override
  } else {
    // Landing on an SD font loads its file from the card — show a brief popup.
    GUI.drawPopup(renderer, tr(STR_LOADING_FONT));
    snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s",
             families[next - CrossPointSettings::BUILTIN_FONT_COUNT].name.c_str());
  }
  ensureSdFontLoaded();
  prewarmSampleGlyphs();
}

void FontLayoutPreviewActivity::prewarmSampleGlyphs() {
  // Built-in glyph bitmaps live in flash (no I/O). SD-card glyphs are read from
  // the card on demand, and without caching every draw re-reads the card and
  // thrashes an 8-slot ring — slow on every render. Prewarm loads all sample
  // glyph bitmaps (regular + bold) into RAM once, so subsequent draws and
  // re-renders after any layout change are I/O-free, like built-in fonts.
  if (SETTINGS.sdFontFamilyName[0] == '\0') return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  std::string all;
  for (const StrId para : kSampleParas) {
    all += I18N.get(para);
    all += ' ';
  }
  fcm->prewarmCache(SETTINGS.getReaderFontId(), all.c_str(), 0x03);
}

void FontLayoutPreviewActivity::activateRow(const Row& row) {
  // The only action row is Book Settings' "Use Global Settings" (Font Download
  // moved to the main Settings list — see the kRows comment).
  if (mode != Mode::Book || row.label != StrId::STR_USE_GLOBAL_SETTINGS) return;
  // Puts every global value back in SETTINGS and removes the book's sidecar,
  // so there is nothing left for onExit() to save.
  BookSettings::reset();
  changed = false;
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    GUI.drawPopup(renderer, tr(STR_LOADING_FONT));
  }
  ensureSdFontLoaded();  // the global font may not be the one that was loaded
  prewarmSampleGlyphs();
  previewDirty = true;
  fullRefreshNext = true;  // the whole sample re-lays-out; avoid ghosting
  if (SETTINGS.textAntiAliasing) aaPreviewNext = true;
  requestUpdate();
}

std::string FontLayoutPreviewActivity::rowValueText(const Row& row) const {
  switch (row.kind) {
    case RowKind::Enum: {
      // Font family shows the active SD font name when one is selected.
      if (row.field == &CrossPointSettings::fontFamily && SETTINGS.sdFontFamilyName[0] != '\0') {
        return SETTINGS.sdFontFamilyName;
      }
      const uint8_t v = SETTINGS.*(row.field);
      if (row.enumLabels && v < row.enumCount) {
        return I18N.get(row.enumLabels[v]);
      }
      return "";
    }
    case RowKind::Toggle:
      return SETTINGS.*(row.field) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case RowKind::Value: {
      std::string text = std::to_string(SETTINGS.*(row.field));
      if (row.field == &CrossPointSettings::wordSpacing) text += '%';
      return text;
    }
    case RowKind::Action:
      return "";
  }
  return "";
}

void FontLayoutPreviewActivity::renderPreview(int x, int y, int w, int h) const {
  const int fontId = SETTINGS.getReaderFontId();

  // Drawn plain here; right after AA is enabled, render() re-runs this same
  // content through the grayscale AA pass (ReaderUtils::renderAntiAliased) so the
  // effect is visible without a hard refresh on every change.

  // SD-card fonts load glyph metrics on demand — prepare the advance table for
  // the whole sample so widths are correct. Bitmaps are cached separately by
  // prewarmSampleGlyphs() when the font/size changes. No-op for built-in fonts.
  for (const StrId para : kSampleParas) {
    renderer.ensureSdCardFontReady(fontId, I18N.get(para), 0x03);
  }

  int lineH = static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression() + 0.5f);
  if (lineH < 8) lineH = 8;

  const int margin = SETTINGS.screenMargin;
  const int textLeft = x + margin;
  int maxWidth = (x + w - margin) - textLeft;
  if (maxWidth < 20) maxWidth = 20;

  const uint8_t align = SETTINGS.paragraphAlignment;
  const bool bionic = SETTINGS.bionicReading != 0;
  // The reader's spacing controls, applied the way the layout engine applies them.
  const TextLayoutOptions textLayout = SETTINGS.readerTextLayout();
  const int spaceW = textLayout.scaleSpace(renderer.getSpaceWidth(fontId));
  // A chosen Paragraph Indent shows as the reader lays it out: N word spaces on left/justified
  // text. "Book's Style" depends on each book's CSS, so it keeps the preview's old stand-in.
  const bool naturalAlign = align == CrossPointSettings::JUSTIFIED || align == CrossPointSettings::LEFT_ALIGN ||
                            align == CrossPointSettings::BOOK_STYLE;
  int indentPx = (align == CrossPointSettings::BOOK_STYLE) ? spaceW * 4 : 0;
  if (textLayout.overridesParagraphIndent()) {
    indentPx = naturalAlign ? textLayout.paragraphIndentSpaces() * spaceW : 0;
  }

  const int regionBottom = y + h;
  int cursorY = y;

  for (const StrId para : kSampleParas) {
    if (cursorY + lineH > regionBottom) break;
    cursorY = renderParagraph(renderer, I18N.get(para), fontId, lineH, textLeft, maxWidth, align, bionic, spaceW,
                              indentPx, cursorY, regionBottom, textLayout.characterSpacing);
    // renderParagraph already advanced one lineH past the last line (normal line
    // spacing). Extra paragraph spacing adds lineHeight/2 on top (matches the
    // reader: ChapterHtmlSlimParser.cpp:1900); off adds nothing.
    if (SETTINGS.extraParagraphSpacing) cursorY += lineH / 2;
  }
}

// Orientation-aware screen split. render() paints from this and loop() hit-tests
// taps against the same numbers, so the two can never drift apart.
FontLayoutPreviewActivity::Layout FontLayoutPreviewActivity::layout() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Reserve space for button hints depending on orientation (mirrors the reader
  // menu): a side gutter in landscape, a top gutter in inverted portrait, and
  // the bottom hint bar in normal portrait.
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isLandscape = isLandscapeCw || isLandscapeCcw;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;

  const int hintGutterW = isLandscape ? metrics.sideButtonHintsWidth : 0;

  Layout l{};
  l.contentX = isLandscapeCw ? hintGutterW : 0;
  l.contentW = pageWidth - hintGutterW;
  const int topGutter = isInverted ? metrics.buttonHintsHeight : 0;
  const int bottomGutter = (isLandscape || isInverted) ? 0 : metrics.buttonHintsHeight;

  l.contentTop = topGutter + metrics.topPadding;
  const int contentBottom = pageHeight - bottomGutter - metrics.topPadding;
  l.previewTop = l.contentTop + 26;

  // Split the remaining content: preview on top (~48%), list below.
  const int available = contentBottom - l.previewTop;
  l.previewH = available * 48 / 100;
  if (l.previewH < 40) l.previewH = 40;

  l.sepY = l.previewTop + l.previewH + metrics.verticalSpacing / 2;
  l.listY = l.sepY + metrics.verticalSpacing / 2;
  l.topH = l.listY;  // region above the list: title + sample + separator
  l.listH = contentBottom - l.listY;
  return l;
}

Rect FontLayoutPreviewActivity::listRect() const {
  const Layout l = layout();
  return Rect{l.contentX, l.listY, l.contentW, l.listH};
}

void FontLayoutPreviewActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const Layout l = layout();
  const int contentX = l.contentX;
  const int contentW = l.contentW;
  const int contentTop = l.contentTop;
  const int previewTop = l.previewTop;
  const int previewH = l.previewH;
  const int sepY = l.sepY;
  const int topH = l.topH;

  renderer.clearScreen();

  // Plain navigation doesn't change the preview, so restore it from the snapshot
  // instead of re-laying-out the sample text (the expensive part). Only re-render
  // and re-snapshot when a setting actually changed (previewDirty).
  bool restored = false;
  if (!previewDirty && previewValid && previewSnap && snapW == pageWidth && snapH == topH) {
    restored = renderer.copyBufferToRegion(0, 0, pageWidth, topH, previewSnap, previewSnapCap);
  }
  if (!restored) {
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop,
                              mode == Mode::Book ? tr(STR_BOOK_SETTINGS) : tr(STR_FONT_LAYOUT_PREVIEW), true,
                              EpdFontFamily::BOLD);
    renderPreview(contentX, previewTop, contentW, previewH);
    renderer.drawLine(contentX + 4, sepY, contentX + contentW - 4, sepY, true);

    const size_t need = renderer.getRegionByteSize(0, 0, pageWidth, topH);
    if (need > previewSnapCap) {
      free(previewSnap);
      previewSnap = static_cast<uint8_t*>(malloc(need));
      previewSnapCap = previewSnap ? need : 0;
    }
    previewValid = previewSnap && renderer.copyRegionToBuffer(0, 0, pageWidth, topH, previewSnap, previewSnapCap);
    snapW = pageWidth;
    snapH = topH;
    previewDirty = false;
  }

  GUI.drawList(
      renderer, listRect(), rowCount, selectedIndex, [this](int i) { return std::string(I18N.get(rowAt(i).label)); },
      nullptr, nullptr,
      [this](int i) {
        const Row& row = rowAt(i);
        std::string text = rowValueText(row);
        // Book Settings: star the rows this book sets itself; the rest follow
        // the global settings.
        if (mode == Mode::Book && row.field && BookSettings::isOverridden(row.field)) text += " *";
        return text;
      },
      true);

  const bool actionSelected = rowAt(selectedIndex).kind == RowKind::Action;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), actionSelected ? tr(STR_SELECT) : tr(STR_CHANGE),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();

  const HalDisplay::RefreshMode mode = fullRefreshNext ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH;
  fullRefreshNext = false;
  renderer.displayBuffer(mode);

  // Anti-aliasing: re-render just the sample text through the grayscale pass so
  // the effect is visible (same mechanism as the reader). This is a hard refresh,
  // so it only runs once — the frame right after AA is enabled — not on every
  // change. The AA LUT is computed for black-on-white, so it is skipped in dark mode.
  if (aaPreviewNext && SETTINGS.textAntiAliasing && !SETTINGS.darkMode) {
    ReaderUtils::renderAntiAliased(renderer, [this, contentX, previewTop, contentW, previewH]() {
      renderPreview(contentX, previewTop, contentW, previewH);
    });
  }
  aaPreviewNext = false;
}
