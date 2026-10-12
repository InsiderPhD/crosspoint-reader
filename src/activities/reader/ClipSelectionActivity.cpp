#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryLookup.h"

namespace {
// Hold Confirm this long in lookup mode to anchor the first word of a phrase.
constexpr unsigned long LOOKUP_ANCHOR_HOLD_MS = 500;
}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WordList wordList,
                                             const int fontId, Section& section, const int startPageInSection,
                                             const int marginTop, const int marginLeft, const int initialCursorIdx,
                                             const bool preAnchored, const bool lookupMode)
    : Activity("ClipSelection", renderer, mappedInput),
      wordList(std::move(wordList)),
      fontId(fontId),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft),
      lookupMode(lookupMode) {
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

void ClipSelectionActivity::finishLookup() {
  // The cursor word, or the anchored range joined with single spaces. Built in
  // a fixed stack buffer rather than through ClipTextBuilder: that assembles the
  // before/after context a clipping needs, which for a lookup is heap spent on
  // text nobody reads. Whole words only, so a selection longer than the buffer
  // (or than any plausible headword) is cut at a word boundary, never mid-word.
  const int from = startMarkIdx == -1 ? cursorIdx : std::min(startMarkIdx, cursorIdx);
  const int to = startMarkIdx == -1 ? cursorIdx : std::max(startMarkIdx, cursorIdx);

  char phrase[DictionaryLookup::MAX_PHRASE_BYTES];
  size_t len = 0;
  int wordsTaken = 0;
  bool joinToPrevious = false;  // previous word ended in a hyphen the line breaker inserted
  for (int i = from; i <= to && wordsTaken < DictionaryLookup::MAX_PHRASE_WORDS; i++) {
    const WordRef& word = wordList.words[i];
    const char* text = wordList.textOf(word);
    if (clipword::hasEmSpacePrefix(text)) text += 3;  // paragraph-indent marker
    if (clipword::isBlank(text)) continue;
    size_t n = strlen(text);
    // "exam-" + "ple" across a line break is one word: drop the hyphen and the space.
    const bool hyphenSplit = word.endsWithInsertedHyphen && text[n - 1] == '-';
    if (hyphenSplit) n--;
    const size_t separator = (len > 0 && !joinToPrevious) ? 1 : 0;
    if (len + separator + n >= sizeof(phrase)) break;
    if (separator) phrase[len++] = ' ';
    memcpy(phrase + len, text, n);
    len += n;
    joinToPrevious = hyphenSplit;
    if (!hyphenSplit) wordsTaken++;
  }
  phrase[len] = '\0';

  setResult(WordPickResult{phrase});
  finish();
}

void ClipSelectionActivity::finishSelection() {
  if (lookupMode) {
    finishLookup();
    return;
  }

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
  if (!lookupMode) {
    if (handleRangeTouch()) return;
  } else {
    // Phrase: drag from the highlighted word to the last word of the phrase.
    if (handleLookupDrag()) return;
    // Dictionary word pick: a hold picks the word under the finger; a tap moves
    // the cursor there, and a second tap on the word under the cursor picks it.
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

  if (lookupMode) {
    // Hold Confirm to anchor the first word of a phrase. Only a press that began
    // on this screen counts: the hold that opened the lookup (a long-press
    // binding in the reader) may still be down when this activity starts.
    if (mappedInput.wasPressed(Button::Confirm)) {
      confirmPressedHere = true;
      anchorHoldTriggered = false;
    }
    if (confirmPressedHere && !anchorHoldTriggered && startMarkIdx == -1 && mappedInput.isPressed(Button::Confirm) &&
        mappedInput.getHeldTime() >= LOOKUP_ANCHOR_HOLD_MS) {
      anchorHoldTriggered = true;
      startMarkIdx = cursorIdx;
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasReleased(Button::Confirm)) {
    confirmPressedHere = false;
    if (anchorHoldTriggered) {
      // This release ends the anchoring hold; it must not also look the word up.
      anchorHoldTriggered = false;
      return;
    }
    // Lookup mode: one press looks up the cursor word, or the anchored phrase.
    if (lookupMode || startMarkIdx != -1) {
      finishSelection();
    } else {
      startMarkIdx = cursorIdx;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
#if FREEINK_DEVICE_X4PRO
      startTapped = false;
#endif
      requestUpdate();
      return;
    }

    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

#if FREEINK_DEVICE_X4PRO
bool ClipSelectionActivity::handleLookupDrag() {
  // Deliberately narrower than handleRangeTouch(): only a contact that lands on
  // the word already under the cursor can become a drag. Any other contact stays
  // what it was before phrases existed (a tap, a hold, or the Back swipe), so
  // sweeping a finger across the text to go back does not start a lookup.
  int lx, ly;
  const bool held = mappedInput.heldTouchPoint(lx, ly);
  if (held) {
    const int idx = clipword::findWordAt(wordList, wordList.words[cursorIdx].pageIdx, lx, ly);
    if (!touchWasHeld) {
      contactStartWord = (idx == cursorIdx) ? idx : -1;
      dragging = false;
    } else if (!dragging && contactStartWord >= 0 && idx >= 0 && idx != contactStartWord) {
      dragging = true;
      startMarkIdx = contactStartWord;
    }
    if (dragging && idx >= 0 && idx != cursorIdx) {
      cursorIdx = idx;
      requestUpdate();
    }
  }
  const bool lifted = touchWasHeld && !held;
  touchWasHeld = held;
  if (lifted && dragging) {
    dragging = false;
    finishSelection();
    return true;
  }
  return dragging;  // nothing else may act on a contact mid-drag
}

bool ClipSelectionActivity::handleRangeTouch() {
  const int page = wordList.words[cursorIdx].pageIdx;
  int lx, ly;

  // Drag: the word the finger came down on is the start, the word under it now
  // is the end, and lifting saves. A contact only becomes a drag once the
  // finger reaches a different word, so a tap (or a wobbly one) stays a tap.
  const bool held = mappedInput.heldTouchPoint(lx, ly);
  if (held) {
    const int idx = clipword::findWordAt(wordList, page, lx, ly);
    if (!touchWasHeld) {
      contactStartWord = idx;
      dragging = false;
    } else if (!dragging && idx >= 0 && contactStartWord >= 0 && idx != contactStartWord) {
      dragging = true;
      startMarkIdx = contactStartWord;
      startTapped = true;
      cursorIdx = contactStartWord;
    }
    // Word gaps report -1: keep the last word until the finger reaches another.
    if (dragging && idx >= 0 && idx != cursorIdx) {
      cursorIdx = idx;
      requestUpdate();
    }
  }
  const bool lifted = touchWasHeld && !held;
  touchWasHeld = held;
  if (lifted && dragging) {
    dragging = false;
    finishSelection();
    return true;
  }
  if (dragging) return true;  // nothing else may act on a contact mid-drag

  // Tap-tap: the first tap marks the start, the next tap marks the end and
  // saves (tapping the start word again saves just that word). A hold in the
  // reader opens this screen with the held word already marked, so there a
  // single tap on another word saves the range -- and tapping the held word
  // itself just confirms it as the start rather than saving one word.
  // A hold here acts like a tap: its lift reports as one.
  if (mappedInput.wasTapPoint(lx, ly)) {
    const int idx = clipword::findWordAt(wordList, page, lx, ly);
    if (idx < 0) return false;
    if (startMarkIdx == -1 || (!startTapped && idx == startMarkIdx)) {
      startMarkIdx = idx;
      cursorIdx = idx;
      startTapped = true;
      requestUpdate();
    } else {
      cursorIdx = idx;
      finishSelection();
    }
    return true;
  }
  return false;
}
#endif

void ClipSelectionActivity::render(RenderLock&&) {
  // Re-render the page the cursor is on (no framebuffer snapshot — see header), then overlay
  // the selection/cursor highlights on top.
  if (!renderSelectionPage(wordList.words[cursorIdx].pageIdx)) return;

  prewarmHighlightedWords();
  drawHighlights();

  const auto confirmLabel = lookupMode ? tr(STR_LOOKUP) : (startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Up/Down side buttons move the cursor by line; label each box with its direction.
  //
  // Rendered in portrait like EpubReaderActivity::renderButtonHints(): the side boxes use
  // portrait-fixed coords (BaseTheme::drawSideButtonHints) but, unlike drawButtonHints, do
  // not force the frame themselves. Left in the reader's landscape frame they land on the
  // physical bottom edge on top of the front hint bar -- hiding its Back label -- and the
  // lower box (y 345..505) runs past the 480px landscape height and is clipped.
  const auto origOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  renderer.setOrientation(origOrientation);

  if (SETTINGS.darkMode) renderer.invertScreen();
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
    // Redrawn over the page's own glyphs, so it must use the page's letter spacing. Read from
    // the setting rather than stored per word: the page was laid out with this value (the
    // section cache is keyed on it) and WordRef is deliberately kept at 14 bytes.
    renderer.drawText(fontId, drawX, word.y, clipword::hasEmSpacePrefix(text) ? text + 3 : text, textBlack, textStyle,
                      SETTINGS.readerTextLayout().characterSpacing);
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
