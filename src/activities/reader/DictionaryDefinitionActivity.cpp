#include "DictionaryDefinitionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "CrossPointSettings.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictHtmlPages.h"
#include "util/Dictionary.h"
#include "util/DictionaryLookup.h"
#include "util/HtmlToPlainText.h"
#include "util/TouchListNav.h"

namespace {

// Longest measurable/drawable span. Wrapped lines stay under the screen width
// (far below this); only pathological unbreakable tokens are split at this cap.
constexpr size_t MAX_LINE_BYTES = 191;

// Body text left/right inset, matching the reader's default feel.
constexpr int SIDE_PADDING = 20;

// Styled-path ceiling: the laid-out Pages keep the whole definition resident
// (TextBlock arenas ≈ text + ~7 bytes/word plus per-line objects), roughly
// doubling the string's footprint while this activity is stacked over the
// reader and word-select. Bigger definitions take the span-based plain-text
// path, which holds no per-page copies.
constexpr size_t MAX_STYLED_HTML_BYTES = 16 * 1024;

// Hold Confirm this long, then release, to close the whole lookup chain and go
// straight back to the page. (Upstream uses a long Back; ours is taken by the
// global hold-Back-for-Home gesture in main.cpp, which fires while held.)
constexpr unsigned long CLOSE_CHAIN_HOLD_MS = 1000;
// How long a "Not found" / "History full" popup stays over the definition.
constexpr unsigned long MESSAGE_DURATION_MS = 1500;

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r'; }

}  // namespace

bool DictionaryDefinitionActivity::History::push(const std::string& q, const int page) {
  const size_t bytes = q.size() + 1;
  if (depth >= MAX_DEPTH || bytes > static_cast<size_t>(POOL_BYTES - poolUsed)) return false;
  memcpy(pool + poolUsed, q.c_str(), bytes);
  entries[depth] = {poolUsed, static_cast<uint16_t>(std::max(0, page))};
  poolUsed = static_cast<uint16_t>(poolUsed + bytes);
  depth++;
  return true;
}

void DictionaryDefinitionActivity::onEnter() {
  Activity::onEnter();
  prepareDefinition();
  requestUpdate();
}

void DictionaryDefinitionActivity::onExit() {
  endSelection();
  history.reset();
  Activity::onExit();
}

void DictionaryDefinitionActivity::prepareDefinition() {
  // Normalize StarDict multi-type separators so the wrap loop and the
  // C-string font APIs below both see the whole definition.
  std::replace(definition.begin(), definition.end(), '\0', '\n');
  if (!(htmlDefinition && definition.size() <= MAX_STYLED_HTML_BYTES && layoutHtmlPages())) {
    definition = htmlToPlainText(definition, DICTIONARY_PLAIN_HEADWORD_MARKER);
    wrapText();
  }
}

void DictionaryDefinitionActivity::releaseDefinition() {
  // swap, not clear(): clear() keeps the capacity, and the whole point is to
  // hand the block back before the next definition is read.
  std::string().swap(definition);
  std::vector<std::unique_ptr<Page>>().swap(pages);
  std::vector<Line>().swap(lines);
  currentPage = 0;
  totalPages = 1;
}

void DictionaryDefinitionActivity::bodyOrigin(int& x, int& y) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  x = (isLandscapeCw ? metrics.sideButtonHintsWidth : 0) + SIDE_PADDING;
  y = (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight;
}

DictionaryDefinitionActivity::BodyArea DictionaryDefinitionActivity::bodyArea() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = isLandscape ? metrics.sideButtonHintsWidth : 0;
  const int topArea = (isInverted ? metrics.buttonHintsHeight : 0) + metrics.topPadding + metrics.headerHeight;
  const int bottomArea = metrics.buttonHintsHeight + metrics.verticalSpacing;
  return {renderer.getScreenWidth() - hintGutterWidth - 2 * SIDE_PADDING,
          renderer.getScreenHeight() - topArea - bottomArea};
}

// Styled path: lay the HTML definition out through the EPUB chapter parser
// into reader-identical Pages. Frees `definition` on success (the page arenas
// own the text); any failure leaves state untouched for the plain-text path.
bool DictionaryDefinitionActivity::layoutHtmlPages() {
  const BodyArea body = bodyArea();
  if (body.width <= 0 || body.height <= 0) return false;
  if (!buildDictionaryHtmlPages(renderer, definition, static_cast<uint16_t>(body.width),
                                static_cast<uint16_t>(body.height), pages)) {
    return false;
  }
  definition.clear();
  definition.shrink_to_fit();
  totalPages = static_cast<int>(pages.size());
  currentPage = 0;
  return true;
}

int DictionaryDefinitionActivity::measureSpan(const int fontId, const char* text, size_t len,
                                              const EpdFontFamily::Style style) const {
  char buf[MAX_LINE_BYTES + 1];
  len = std::min(len, MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, style);
}

// Greedy word-wrap of `definition` into byte spans. '\n' breaks lines (blank
// lines survive as paragraph spacing; NULs from multi-type StarDict entries
// were normalized to newlines in onEnter); '\r' is dropped by treating it as
// a space at a token edge.
void DictionaryDefinitionActivity::wrapText() {
  lines.clear();
  lines.reserve(definition.size() / 32 + 8);

  const int fontId = SETTINGS.getReaderFontId();
  // SD-card fonts: merge every definition codepoint into the persistent
  // advance table up front. Otherwise each unseen codepoint measured below
  // falls back to an on-demand glyph load from SD (8-slot overflow ring).
  renderer.ensureSdCardFontReady(fontId, definition.c_str(), 0x03 /* REGULAR + BOLD (labels) */);

  const BodyArea body = bodyArea();
  const int maxWidth = body.width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, body.height / lineHeight);

  const char* text = definition.c_str();
  const uint32_t n = static_cast<uint32_t>(definition.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;
  bool lineBold = false;
  const auto style = [&lineBold] { return lineBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR; };

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart), lineBold});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
    lineBold = false;
  };

  uint32_t i = 0;
  while (i < n) {
    // A marker opening a source line makes that line (and its wrapped
    // continuation) bold: an entry's headword label, or a heading.
    if (text[i] == DICTIONARY_PLAIN_HEADWORD_MARKER && lineEnd == lineStart && i == lineStart) {
      lineStart = lineEnd = ++i;
      lineBold = true;
      continue;
    }
    const char c = text[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n' && text[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last
    // complete codepoint so measure/draw never see a partial sequence. A
    // natural stop lands on whitespace or the terminating NUL, never on a
    // continuation byte, so this is a no-op there.
    while (i - tokenStart > 1 && (text[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(fontId, text + tokenStart, tokenLen, style());

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      const bool bold = lineBold;
      flushLine(tokenStart);
      lineBold = bold;  // a wrapped label stays bold
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the screen is now alone on the line
    // (any previous content was flushed above): split it at the widest
    // fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (text[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(fontId, text + lineStart, f, style()) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole
        // UTF-8 sequence rather than splitting it into invalid fragments.
        lastFit = 1;
        while (lastFit < len && (text[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      const bool bold = lineBold;
      flushLine(rest);
      lineBold = bold;
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(fontId, text + lineStart, lineEnd - lineStart, style());
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  currentPage = 0;
}

void DictionaryDefinitionActivity::showMessage(const StrId which) {
  message = which;
  messageVisible = true;
  messageTime = millis();
  requestUpdate();
}

bool DictionaryDefinitionActivity::buildPageWords() {
  std::vector<WordRef>().swap(pageWords.words);
  std::string().swap(pageWords.textPool);
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId);
  int originX = 0;
  int originY = 0;
  bodyOrigin(originX, originY);

  // Counting pass first, so the word vector and the text pool each take one
  // exact reservation rather than growing by doubling (CLAUDE.md rule 7). One
  // page of a definition is at most a few hundred words: ~14 bytes per word
  // plus its text, a few KB held only while the selection is up.
  size_t wordCount = 0;
  size_t poolBytes = 0;
  const auto countToken = [&](size_t len) {
    wordCount++;
    poolBytes += len + 1;
  };
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  if (!pages.empty()) {
    for (const auto& element : pages[currentPage]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& block = static_cast<const PageLine&>(*element).getBlock();
      if (!block) continue;
      for (uint16_t i = 0; i < block->wordCount(); i++) {
        if (!clipword::isBlank(block->wordText(i))) countToken(block->wordTextLen(i));
      }
    }
  } else {
    for (int l = firstLine; l < lastLine; l++) {
      const char* text = definition.c_str() + lines[l].start;
      size_t i = 0;
      while (i < lines[l].len) {
        if (isSpace(text[i])) {
          i++;
          continue;
        }
        const size_t from = i;
        while (i < lines[l].len && !isSpace(text[i])) i++;
        countToken(i - from);
      }
    }
  }
  if (wordCount == 0) return false;
  if (poolBytes > UINT16_MAX) return false;  // WordRef::textOffset; unreachable for one page
  pageWords.words.reserve(wordCount);
  pageWords.textPool.reserve(poolBytes);

  const auto addWord = [this](const char* text, size_t len, int x, int y, int w, int h, EpdFontFamily::Style style) {
    WordRef word;
    word.textOffset = static_cast<uint16_t>(pageWords.textPool.size());
    word.x = static_cast<int16_t>(x);
    word.y = static_cast<int16_t>(y);
    word.w = static_cast<int16_t>(std::max(1, w));
    word.h = static_cast<int16_t>(h);
    word.style = style;
    pageWords.textPool.append(text, len);
    pageWords.textPool.push_back('\0');
    pageWords.words.push_back(word);
  };

  if (!pages.empty()) {
    for (const auto& element : pages[currentPage]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block) continue;
      // A block may be laid out in its own font (a sized heading on PSRAM
      // boards): the selection box has to cover the word as drawn.
      const int fontOverride = block->getBlockStyle().fontOverride;
      const int wordFont = fontOverride != 0 ? fontOverride : fontId;
      const int wordHeight = fontOverride != 0 ? renderer.getLineHeight(wordFont) : lineHeight;
      const uint16_t count = block->wordCount();
      for (uint16_t i = 0; i < count; i++) {
        const char* text = block->wordText(i);
        if (clipword::isBlank(text)) continue;
        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        int width = renderer.getTextAdvanceX(wordFont, text, style, block->getBlockStyle().characterSpacing);
        if (i + 1 < count && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }
        addWord(text, block->wordTextLen(i), originX + line.xPos + block->wordXpos(i), originY + line.yPos, width,
                wordHeight, style);
      }
    }
  } else {
    for (int l = firstLine; l < lastLine; l++) {
      const char* text = definition.c_str() + lines[l].start;
      const auto style = lines[l].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int y = originY + (l - firstLine) * lineHeight;
      size_t i = 0;
      while (i < lines[l].len) {
        if (isSpace(text[i])) {
          i++;
          continue;
        }
        const size_t from = i;
        while (i < lines[l].len && !isSpace(text[i])) i++;
        // Measured from the line start, spaces included, exactly as drawBody
        // draws the span.
        const int x = originX + measureSpan(fontId, text, from, style);
        addWord(text + from, i - from, x, y, measureSpan(fontId, text + from, i - from, style), lineHeight, style);
      }
    }
  }
  return true;
}

void DictionaryDefinitionActivity::startSelection() {
  if (!buildPageWords()) return;
  selecting = true;
  cursor = 0;
  requestUpdate();
}

void DictionaryDefinitionActivity::endSelection() {
  selecting = false;
  std::vector<WordRef>().swap(pageWords.words);
  std::string().swap(pageWords.textPool);
}

void DictionaryDefinitionActivity::moveCursorLine(const int direction) {
  const auto& words = pageWords.words;
  const int n = static_cast<int>(words.size());
  const int y = words[cursor].y;
  int i = cursor;
  if (direction > 0) {
    while (i < n && words[i].y == y) i++;
    if (i < n) cursor = i;
  } else {
    while (i >= 0 && words[i].y == y) i--;
    if (i < 0) return;
    // First word of that previous line.
    const int prevY = words[i].y;
    while (i > 0 && words[i - 1].y == prevY) i--;
    cursor = i;
  }
  requestUpdate();
}

bool DictionaryDefinitionActivity::loadQuery(const char* q, StrId* why) {
  // Release first: two definitions (up to 64KB each, plus a styled layout)
  // are never resident together.
  releaseDefinition();
  auto outcome = lookup->run(renderer, q);
  if (!outcome.found) {
    if (why) *why = outcome.message;
    return false;
  }
  headword = std::move(outcome.headword);
  definition = std::move(outcome.definition);
  htmlDefinition = outcome.html;
  prepareDefinition();
  return true;
}

void DictionaryDefinitionActivity::lookUpWord(const std::string& word) {
  if (!canLookUp() || word.empty()) return;
  if (!history) {
    history = makeUniqueNoThrow<History>();
    if (!history) {
      LOG_ERR("DICT", "OOM: %u byte lookup history", static_cast<unsigned>(sizeof(History)));
      showMessage(StrId::STR_DICT_LOW_MEMORY);
      return;
    }
  }
  const int page = currentPage;
  if (!history->push(query, page)) {
    showMessage(StrId::STR_DICT_HISTORY_FULL);
    return;
  }
  endSelection();

  // Paint the busy popup over the current definition BEFORE the lookup: the
  // lookup borrows the framebuffer as its inflate window (see
  // DictionaryLookup::run), and render() repaints everything after it.
  busy = true;
  requestUpdateAndWait();

  bool closeChain = false;
  {
    RenderLock lock(*this);
    busy = false;
    StrId why = StrId::STR_DICT_NOT_FOUND;
    if (loadQuery(word.c_str(), &why)) {
      query = word;
    } else {
      history->pop();
      // Bring the definition we came from back, on the page it was on.
      if (loadQuery(query.c_str(), nullptr)) {
        currentPage = std::min(page, totalPages - 1);
        message = why;
        messageVisible = true;
        messageTime = millis();
      } else {
        closeChain = true;
      }
    }
  }
  if (closeChain) {
    LOG_ERR("DICT", "Could not restore the previous definition; closing");
    finish();
    return;
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::goBack() {
  if (!history || history->depth == 0) {
    finish();
    return;
  }
  const std::string previous = history->topQuery();
  const int page = history->topPage();
  busy = true;
  requestUpdateAndWait();

  bool restored = false;
  {
    RenderLock lock(*this);
    busy = false;
    history->pop();
    restored = loadQuery(previous.c_str(), nullptr);
    if (restored) {
      query = previous;
      currentPage = std::min(page, totalPages - 1);
    }
  }
  if (!restored) {
    LOG_ERR("DICT", "Could not re-read the previous definition; closing");
    finish();
    return;
  }
  requestUpdate();
}

void DictionaryDefinitionActivity::loop() {
  if (messageVisible && millis() - messageTime >= MESSAGE_DURATION_MS) {
    messageVisible = false;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selecting) {
      endSelection();
      requestUpdate();
    } else {
      goBack();
    }
    return;
  }

  if (selecting) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      lookUpWord(pageWords.textOf(pageWords.words[cursor]));
      return;
    }
#if FREEINK_CAP_TOUCH
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasTapPoint(tx, ty)) {
      const int idx = clipword::findWordAt(pageWords, 0, tx, ty);
      if (idx >= 0) lookUpWord(pageWords.textOf(pageWords.words[idx]));
      return;
    }
#endif
    const int n = static_cast<int>(pageWords.words.size());
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, n] {
      cursor = ButtonNavigator::nextIndex(cursor, n);
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, n] {
      cursor = ButtonNavigator::previousIndex(cursor, n);
      requestUpdate();
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveCursorLine(+1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveCursorLine(-1); });
    return;
  }

  // Fires while still held (like the reader's own holds), so the press never
  // reaches the release that would start a selection; the reader's onResume()
  // arms the held Confirm, so its release does not open the reader menu either.
  if (history && history->depth > 0 && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= CLOSE_CHAIN_HOLD_MS) {
    finish();  // the whole chain: this one screen holds every level
    return;
  }
  if (canLookUp() && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    startSelection();
    return;
  }

#if FREEINK_CAP_TOUCH
  // Hold on a word: look it up straight away, the reader's own gesture.
  int hx = 0;
  int hy = 0;
  if (canLookUp() && mappedInput.wasTouchLongPressPoint(hx, hy)) {
    if (buildPageWords()) {
      const int idx = clipword::findWordAt(pageWords, 0, hx, hy);
      const std::string word = idx >= 0 ? pageWords.textOf(pageWords.words[idx]) : std::string();
      endSelection();
      lookUpWord(word);
    }
    return;
  }

  // Full Touch: a vertical swipe pages the definition too, content-drag sense
  // (swipe up reveals what is below), matching every other scrollable surface.
  // The currentPage bounds either side are the check pageSwipeDelta requires.
  switch (TouchListNav::pageSwipeDelta(mappedInput)) {
    case +1:
      if (currentPage + 1 < totalPages) {
        currentPage++;
        requestUpdate();
      }
      return;
    case -1:
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
      return;
    default:
      break;
  }

  // Same tap zones as the reader page turns: left third = previous page,
  // the rest = next. Back is the usual left-edge swipe. Fenced because the tap
  // queries only exist on the touch board — see MappedInputManager.h.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasTapPoint(tx, ty)) {
    if (tx < renderer.getScreenWidth() / 3) {
      if (currentPage > 0) {
        currentPage--;
        requestUpdate();
      }
    } else if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
    return;
  }
#endif

  buttonNavigator.onNext([this] {
    if (currentPage + 1 < totalPages) {
      currentPage++;
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
  });
}

// Draws the current page: a styled Page when the HTML layout succeeded,
// otherwise the wrapped line spans (copied into a stack buffer for NUL
// termination). Called twice per render: once in font-cache scan mode, once
// for the real paint.
void DictionaryDefinitionActivity::drawBody(const int fontId, const int x, const int startY) const {
  if (!pages.empty()) {
    pages[currentPage]->render(renderer, fontId, x, startY);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int firstLine = currentPage * linesPerPage;
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, definition.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, startY + (i - firstLine) * lineHeight, buf, true,
                      lines[i].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

void DictionaryDefinitionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.sideButtonHintsWidth : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = renderer.getScreenWidth() - hintGutterWidth;
  const int contentY = isInverted ? metrics.buttonHintsHeight : 0;

  // Header: matched headword left, page counter right.
  const int headerY = contentY + metrics.topPadding + 10;
  renderer.drawText(UI_12_FONT_ID, contentX + SIDE_PADDING, headerY, headword.c_str(), true, EpdFontFamily::BOLD);
  if (totalPages > 1) {
    char counter[16];
    snprintf(counter, sizeof(counter), "%d/%d", currentPage + 1, totalPages);
    const int counterWidth = renderer.getTextWidth(UI_10_FONT_ID, counter);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - SIDE_PADDING - counterWidth, headerY, counter);
  }

  // Body: two-pass draw inside a prewarm scope (same pattern as the reader's
  // renderContents) so SD-card font glyphs load from SD in one batch instead
  // of one on-demand overflow read per character on every page turn.
  const int fontId = SETTINGS.getReaderFontId();
  const int bodyStartY = contentY + metrics.topPadding + metrics.headerHeight;
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);  // scan pass: records codepoints only
  scope.endScanAndPrewarm();
  drawBody(fontId, contentX + SIDE_PADDING, bodyStartY);

  if (selecting && cursor >= 0 && cursor < static_cast<int>(pageWords.words.size())) {
    const WordRef& word = pageWords.words[cursor];
    renderer.invertRect(word.x, word.y, word.w, word.h);
  }

  const auto labels =
      selecting ? mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_LOOKUP), "<", ">")
                : mappedInput.mapLabels(tr(STR_BACK), canLookUp() ? tr(STR_SELECT) : "", (currentPage > 0 ? "<" : ""),
                                        (currentPage + 1 < totalPages ? ">" : ""));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // drawPopup() inverts for dark mode and pushes the frame itself.
  if (busy) {
    GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
    return;
  }
  if (messageVisible) {
    // I18N.get, not tr(): the message is chosen at runtime.
    GUI.drawPopup(renderer, I18N.get(message));
    return;
  }
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
