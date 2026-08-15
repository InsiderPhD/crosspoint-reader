#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WordList wordList,
                                             const int fontId, Section& section, const int startPageInSection,
                                             const int marginTop, const int marginLeft, const int initialCursorIdx,
                                             const bool preAnchored)
    : Activity("ClipSelection", renderer, mappedInput),
      wordList(std::move(wordList)),
      fontId(fontId),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft) {
  const int wordCount = static_cast<int>(this->wordList.words.size());
  if (initialCursorIdx > 0 && initialCursorIdx < wordCount) {
    cursorIdx = initialCursorIdx;
  }
  if (preAnchored) {
    startMarkIdx = cursorIdx;
  }
}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();

  if (wordList.words.empty()) {
    LOG_ERR("CLIP", "No words available for selection");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  savedSectionPage = section.currentPage;
  currentDisplayPage = wordList.words[cursorIdx].pageIdx;
  requestUpdate();
}

void ClipSelectionActivity::onExit() {
  section.currentPage = savedSectionPage;
  Activity::onExit();
}

void ClipSelectionActivity::finishSelection() {
  // A touch finish can land before any anchor was set (tap-to-select without a
  // prior hold): treat the cursor word as a single-word selection.
  if (startMarkIdx == -1) {
    startMarkIdx = cursorIdx;
  }
  const int total = static_cast<int>(wordList.words.size());
  const int from = std::min(startMarkIdx, cursorIdx);
  const int to = std::max(startMarkIdx, cursorIdx);
  auto result = ClipTextBuilder::build(wordList, from, to, total, startPageInSection, section.pageCount);
  if (const auto paragraphIndex = section.getParagraphIndexForPage(result.sectionPage)) {
    result.paragraphIndex = *paragraphIndex;
  }
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(wordList.words.size());
  using Button = MappedInputManager::Button;

  auto moveCursor = [this](const int nextIdx) {
    if (nextIdx == cursorIdx) return;
    cursorIdx = nextIdx;
    requestUpdate();
  };

  buttonNavigator.onRelease({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onContinuous({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onRelease({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onContinuous({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onRelease({Button::Down}, [this, &moveCursor] { moveCursor(lineForward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Down}, [this, &moveCursor] { moveCursor(lineForward(cursorIdx)); });
  buttonNavigator.onRelease({Button::Up}, [this, &moveCursor] { moveCursor(lineBackward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Up}, [this, &moveCursor] { moveCursor(lineBackward(cursorIdx)); });

#if FREEINK_DEVICE_X4PRO
  // Kindle-style touch selection. A hold picks the word under the finger and
  // finishes there (so hold-at-start + hold-at-end makes a clipping in two
  // gestures); a tap extends the selection to that word, and a second tap on
  // the word already under the cursor finishes.
  {
    int lx, ly;
    if (mappedInput.wasTouchLongPressPoint(lx, ly)) {
      const int idx = clipword::findWordAt(wordList, wordList.words[cursorIdx].pageIdx, lx, ly);
      if (idx >= 0) {
        mappedInput.suppressTouchContact();  // the lift must not also tap
        cursorIdx = idx;
        finishSelection();
        return;
      }
    }
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int idx = clipword::findWordAt(wordList, wordList.words[cursorIdx].pageIdx, lx, ly);
      if (idx >= 0) {
        if (idx != cursorIdx) {
          cursorIdx = idx;
          requestUpdate();
        } else {
          finishSelection();
        }
        return;
      }
    }
  }
#endif

  if (mappedInput.wasReleased(Button::Confirm)) {
    if (startMarkIdx == -1) {
      startMarkIdx = cursorIdx;
      requestUpdate();
    } else {
      finishSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      requestUpdate();
      return;
    }

    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  // Re-render the page the cursor is on (no framebuffer snapshot — see header), then overlay
  // the selection/cursor highlights on top.
  if (!renderSelectionPage(wordList.words[cursorIdx].pageIdx)) return;

  prewarmHighlightedWords();
  drawHighlights();

  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Up/Down side buttons move the cursor by line; label each box with its direction.
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}

bool ClipSelectionActivity::renderSelectionPage(const int pageIdx) {
  // Reload from the section file only when the cursor crosses to a new page; otherwise reuse
  // the cached Page so a same-page move just re-blits glyphs (no SD read / deserialize).
  if (pageIdx != currentDisplayPage || !cachedPage) {
    const int oldPage = section.currentPage;
    section.currentPage = startPageInSection + pageIdx;
    cachedPage = section.loadPageFromSectionFile();
    section.currentPage = oldPage;
    if (!cachedPage) {
      LOG_ERR("CLIP", "Failed to load selection page %d", pageIdx);
      return false;
    }
    currentDisplayPage = pageIdx;
  }

  renderer.clearScreen();
  cachedPage->render(renderer, fontId, marginLeft, marginTop);
  return true;
}

void ClipSelectionActivity::applyWordStyle(const WordRef& word, const ClipWordStyle& style) const {
  const char* text = wordList.textOf(word);
  const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  const int skipX = clipword::hasEmSpacePrefix(text) ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
  const int drawX = word.x + skipX;
  const int drawW = word.w - skipX;
  if (drawW <= 0) return;

  const bool invert = (style.flags & ClipWordStyle::INVERT) != 0;
  const bool fill = !invert && (style.flags & ClipWordStyle::FILL) != 0;
  if (invert) {
    renderer.fillRect(drawX, word.y, drawW, word.h, true);
  } else if (fill) {
    renderer.fillRectDither(drawX, word.y, drawW, word.h, style.fillColor);
  }

  if ((style.flags & ClipWordStyle::BORDER) != 0) {
    renderer.drawRect(drawX, word.y, drawW, word.h, !invert);
  }

  if (!clipword::isBlank(text)) {
    const bool textBlack = !invert;
    renderer.drawText(fontId, drawX, word.y, clipword::hasEmSpacePrefix(text) ? text + 3 : text, textBlack, textStyle);
  }

  if ((style.flags & ClipWordStyle::UNDERLINE) != 0) {
    const int underlineY = word.y + renderer.getFontAscenderSize(fontId) + 2;
    renderer.drawLine(drawX, underlineY, drawX + drawW, underlineY, true);
  }
}

void ClipSelectionActivity::prewarmHighlightedWords() const {
  if (!renderer.isSdCardFont(fontId)) return;

  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  for (auto& text : prewarmTextByStyle) {
    text.clear();
  }

  const auto appendWord = [this](const WordRef& word) {
    if (word.pageIdx != currentDisplayPage) return;
    const uint8_t styleIdx = static_cast<uint8_t>(word.style) & 0x03;
    if (styleIdx >= prewarmTextByStyle.size()) return;
    prewarmTextByStyle[styleIdx] += wordList.textOf(word);
    prewarmTextByStyle[styleIdx].push_back(' ');
  };

  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; i++) {
      appendWord(wordList.words[i]);
    }
  }

  appendWord(wordList.words[cursorIdx]);

  for (uint8_t styleIdx = 0; styleIdx < prewarmTextByStyle.size(); styleIdx++) {
    if (!prewarmTextByStyle[styleIdx].empty()) {
      fcm->prewarmCache(fontId, prewarmTextByStyle[styleIdx].c_str(), static_cast<uint8_t>(1u << styleIdx));
    }
  }
}

void ClipSelectionActivity::drawHighlights() {
  static constexpr ClipWordStyle selectionStyle{ClipWordStyle::FILL | ClipWordStyle::UNDERLINE, Color::LightGray};
  static constexpr ClipWordStyle cursorStyle{ClipWordStyle::INVERT, Color::LightGray};

  const auto& words = wordList.words;
  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; i++) {
      if (words[i].pageIdx == currentDisplayPage) {
        applyWordStyle(words[i], selectionStyle);
      }
    }
  }

  if (words[cursorIdx].pageIdx == currentDisplayPage) {
    applyWordStyle(words[cursorIdx], cursorStyle);
  }
}

int ClipSelectionActivity::lineForward(const int idx) const {
  const auto& words = wordList.words;
  const int total = static_cast<int>(words.size());
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;
  for (int i = idx + 1; i < total; ++i) {
    if (words[i].pageIdx != page || words[i].y != lineY) return i;  // first word of the next line
  }
  return idx;
}

int ClipSelectionActivity::lineBackward(const int idx) const {
  const auto& words = wordList.words;
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;
  int i = idx - 1;
  for (; i >= 0; --i) {
    if (words[i].pageIdx != page || words[i].y != lineY) break;  // step onto the previous line
  }
  if (i < 0) return idx;

  const int prevY = words[i].y;
  const int prevPage = words[i].pageIdx;
  int first = i;
  for (; i >= 0; --i) {
    if (words[i].pageIdx != prevPage || words[i].y != prevY) break;
    first = i;  // walk back to that line's first word
  }
  return first;
}
