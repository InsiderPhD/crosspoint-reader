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
                                             const bool preAnchored, const bool lookupMode, const bool adoptContact,
                                             const bool selectMode, const bool lookupAvailable)
    : Activity("ClipSelection", renderer, mappedInput),
      wordList(std::move(wordList)),
      fontId(fontId),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft),
      lookupMode(lookupMode),
      selectMode(selectMode),
      lookupAvailable(lookupAvailable) {
  const int wordCount = static_cast<int>(this->wordList.words.size());
  if (initialCursorIdx > 0 && initialCursorIdx < wordCount) {
    cursorIdx = initialCursorIdx;
  }
  if (preAnchored) {
    startMarkIdx = cursorIdx;
  }
#if FREEINK_DEVICE_X4PRO
  if (adoptContact) {
    // Seed the drag tracker as though it had already seen this contact land on
    // the cursor word: touchWasHeld stops the first frame from re-reading the
    // start word from wherever the finger has drifted to, and makes a contact
    // that ended while the reader was building the word list read as a lift.
    contactAdopted = true;
    touchWasHeld = true;
    contactStartWord = cursorIdx;
  }
#else
  (void)adoptContact;
#endif
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

#if FREEINK_DEVICE_X4PRO
  // Ahead of the buttons: while the finger that opened this screen is still
  // down, a side key or page-turn remote must not move the cursor out from
  // under the word it is holding.
  if (handleAdoptedContact()) return;
  if (selectMode && handleSelectTouch()) return;
#endif

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
  if (selectMode) {
    // Touch is handleSelectTouch()'s alone; the buttons below still work.
  } else if (!lookupMode) {
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
    // Select mode always has a range (there is no unanchored state to step
    // back to), so Back leaves.
    if (startMarkIdx != -1 && !selectMode) {
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
bool ClipSelectionActivity::handleAdoptedContact() {
  if (!contactAdopted) return false;

  // Same tracking as the two drag handlers below, with the start word fixed by
  // the reader's hold instead of read from the first frame.
  int lx, ly;
  const bool held = mappedInput.heldTouchPoint(lx, ly);
  if (held) {
    const int idx = clipword::findWordAt(wordList, wordList.words[cursorIdx].pageIdx, lx, ly);
    if (!dragging && idx >= 0 && idx != contactStartWord) {
      dragging = true;
      startMarkIdx = contactStartWord;
    }
    // Word gaps report -1: keep the last word until the finger reaches another.
    if (dragging && idx >= 0 && idx != cursorIdx) {
      cursorIdx = idx;
      requestUpdate();
    }
    return true;
  }

  // The contact is over as far as this screen can tell. Besides a lift,
  // heldTouchPoint() also goes false when a second finger lands or the finger
  // slides onto an action-bar button, with the first finger still down. Drop
  // whatever is left of the contact in every case: its eventual lift must not
  // come back as a tap on a word, an action-bar press, or (once this screen has
  // finished, in the same frame) a tap zone in the reader.
  mappedInput.suppressTouchContact();
  contactAdopted = false;
  touchWasHeld = false;
  // A drag that came back to the word it started on is a finger that wandered
  // during the hold, not a range.
  const bool draggedRange = dragging && cursorIdx != contactStartWord;
  dragging = false;
  if (selectMode) {
    // Lifting never finishes here: it settles the range and brings up the
    // handles and the toolbar.
    if (!draggedRange) startMarkIdx = cursorIdx = contactStartWord;
    requestUpdate();
    return true;
  }
  // Lookup: lifting is the lookup, of the held word or the dragged phrase.
  // Clip: lifting after a drag saves, as a drag begun on this screen does; with
  // no drag the held word stays anchored (as it is after a Shortcut-mode hold)
  // for a tap, a new drag or the buttons to finish.
  if (lookupMode || draggedRange) finishSelection();
  return true;
}

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

// --- Select mode -----------------------------------------------------------
namespace {
// Logical pixels. The hit box keeps a handle around 7 mm wide on the X4 Pro.
constexpr int STEM_W = 3;
constexpr int DOT_SIZE = 16;
constexpr int HANDLE_HIT = 60;
constexpr int BAR_MIN_H = 56;
constexpr int BAR_PAD_Y = 10;
constexpr int SEGMENT_MIN_W = 116;
constexpr int SEGMENT_PAD_X = 16;
constexpr int BORDER = 2;
constexpr int RADIUS = 4;
constexpr int DIVIDER_INSET = 10;
constexpr int POINTER_HALF_W = 12;
constexpr int POINTER_H = 12;
constexpr int SELECTION_GAP = 6;
constexpr int BAR_FONT_ID = UI_12_FONT_ID;

const char* selectActionLabel(const int action) { return action == 0 ? tr(STR_LOOKUP) : tr(STR_CLIP); }
}  // namespace

ClipSelectionActivity::Box ClipSelectionActivity::wordBox(const int idx) const {
  const WordRef& word = wordList.words[idx];
  const char* text = wordList.textOf(word);
  const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  // The paragraph-indent em space is not part of the word as seen.
  const int skipX = clipword::hasEmSpacePrefix(text) ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
  return {word.x + skipX, word.y, std::max(1, word.w - skipX), word.h};
}

ClipSelectionActivity::SelectLayout ClipSelectionActivity::selectLayout() const {
  SelectLayout layout;
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const Box safe{left, top, renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom};
  if (safe.w <= DOT_SIZE || safe.h <= DOT_SIZE) return layout;

  const int from = startMarkIdx == -1 ? cursorIdx : std::min(startMarkIdx, cursorIdx);
  const int to = startMarkIdx == -1 ? cursorIdx : std::max(startMarkIdx, cursorIdx);
  const Box first = wordBox(from);
  const Box last = wordBox(to);

  // Handles: a stem along the selection's outer edge, a dot past the line
  // (above the start, below the end). The hit box is split at the word's
  // middle, so on a one-word selection the two never overlap.
  const auto makeHandle = [&safe](const Box& word, const bool isStart) {
    SelectHandle h;
    const int stemX = std::clamp(isStart ? word.x - STEM_W : word.x + word.w, safe.x, safe.x + safe.w - STEM_W);
    const int stemTop = std::max(safe.y, word.y - 2);
    const int stemBottom = std::min(safe.y + safe.h, word.y + word.h + 2);
    h.stem = {stemX, stemTop, STEM_W, std::max(1, stemBottom - stemTop)};
    const int midX = stemX + STEM_W / 2;
    const int dotX = std::clamp(midX - DOT_SIZE / 2, safe.x, safe.x + safe.w - DOT_SIZE);
    const int dotY = std::clamp(isStart ? stemTop - DOT_SIZE + 1 : stemBottom - 1, safe.y, safe.y + safe.h - DOT_SIZE);
    h.dot = {dotX, dotY, DOT_SIZE, DOT_SIZE};
    const int wordMid = word.y + word.h / 2;
    const int dotMid = dotY + DOT_SIZE / 2;
    const int hitTop = isStart ? std::min(dotMid - HANDLE_HIT / 2, dotY) : wordMid;
    const int hitBottom = isStart ? wordMid : std::max(dotMid + HANDLE_HIT / 2, dotY + DOT_SIZE);
    h.hit = {midX - HANDLE_HIT / 2, hitTop, HANDLE_HIT, hitBottom - hitTop};
    return h;
  };
  layout.start = makeHandle(first, true);
  layout.end = makeHandle(last, false);

  // Toolbar: Look Up (only with a dictionary set) | Clip.
  const bool visible[ACTION_COUNT] = {lookupAvailable, true};
  int widths[ACTION_COUNT] = {};
  int barW = 0;
  for (int i = 0; i < ACTION_COUNT; ++i) {
    if (!visible[i]) continue;
    const int labelW = renderer.getTextWidth(BAR_FONT_ID, selectActionLabel(i), EpdFontFamily::BOLD);
    widths[i] = std::max(SEGMENT_MIN_W, labelW + 2 * SEGMENT_PAD_X);
    barW += widths[i];
  }
  if (barW > safe.w) {
    for (int& w : widths) w = w * safe.w / barW;
    barW = safe.w;
  }
  const int barH = std::min(safe.h, std::max(BAR_MIN_H, renderer.getLineHeight(BAR_FONT_ID) + 2 * BAR_PAD_Y));

  const int selTop = std::min(first.y, layout.start.dot.y);
  const int selBottom = std::max(last.y + last.h, layout.end.dot.y + layout.end.dot.h);
  const int reach = SELECTION_GAP + POINTER_H;
  const auto clampX = [&](const int x) { return std::clamp(x, safe.x, safe.x + safe.w - barW); };
  int anchorX = first.x + first.w / 2;
  int barX = clampX(anchorX - barW / 2);
  int barY;
  if (selTop - reach - barH >= safe.y) {
    // 1. Above the selection, pointing down at its first word.
    barY = selTop - reach - barH;
    layout.showPointer = true;
  } else if (selBottom + reach + barH <= safe.y + safe.h) {
    // 2. Below it, pointing up at its last word.
    barY = selBottom + reach;
    anchorX = last.x + last.w / 2;
    barX = clampX(anchorX - barW / 2);
    layout.showPointer = true;
    layout.pointerUp = true;
  } else {
    // 3. The selection fills the page: cover the end of it that hides less,
    //    sliding sideways off any handle so a handle press never hits the bar.
    const auto covered = [&](const int y) { return std::max(0, std::min(y + barH, selBottom) - std::max(y, selTop)); };
    const int topY = safe.y;
    const int bottomY = safe.y + safe.h - barH;
    barY = covered(bottomY) < covered(topY) ? bottomY : topY;
    const auto hidesHandle = [&](const int x) {
      const Box bar{x, barY, barW, barH};
      for (const auto* h : {&layout.start, &layout.end}) {
        for (const auto* b : {&h->dot, &h->stem}) {
          if (b->x < bar.x + bar.w && bar.x < b->x + b->w && b->y < bar.y + bar.h && bar.y < b->y + b->h) return true;
        }
      }
      return false;
    };
    for (const int x : {barX, clampX(layout.start.hit.x + layout.start.hit.w), clampX(layout.start.hit.x - barW),
                        clampX(layout.end.hit.x + layout.end.hit.w), clampX(layout.end.hit.x - barW)}) {
      if (!hidesHandle(x)) {
        barX = x;
        break;
      }
    }
  }
  layout.bar = {barX, barY, barW, barH};
  const int pointerMargin = RADIUS + BORDER + POINTER_HALF_W;
  layout.pointerX = barW > 2 * pointerMargin ? std::clamp(anchorX, barX + pointerMargin, barX + barW - pointerMargin)
                                             : barX + barW / 2;
  int x = barX;
  for (int i = 0; i < ACTION_COUNT; ++i) {
    if (!visible[i]) continue;
    layout.actions[i] = {x, barY, widths[i], barH};
    x += widths[i];
  }
  return layout;
}

void ClipSelectionActivity::drawSelectOverlay(const SelectLayout& layout, const bool showActions) const {
  for (const auto* h : {&layout.start, &layout.end}) {
    renderer.fillRect(h->stem.x, h->stem.y, h->stem.w, h->stem.h, true);
    renderer.fillRoundedRect(h->dot.x, h->dot.y, h->dot.w, h->dot.h, DOT_SIZE / 2, Color::Black);
  }
  if (!showActions || layout.bar.w <= 0) return;

  const Box& bar = layout.bar;
  renderer.fillRoundedRect(bar.x, bar.y, bar.w, bar.h, RADIUS, Color::White);
  renderer.drawRoundedRect(bar.x, bar.y, bar.w, bar.h, BORDER, RADIUS, true);
  if (layout.showPointer) {
    // A black triangle from inside the border, then a white one inside it that
    // erases that stretch of border, so the outline runs unbroken.
    const int dir = layout.pointerUp ? -1 : 1;
    const int edge = layout.pointerUp ? bar.y : bar.y + bar.h;
    const int baseY = edge - dir * BORDER;
    const int tipY = edge + dir * POINTER_H;
    const int px = layout.pointerX;
    const int outerX[] = {px - POINTER_HALF_W, px + POINTER_HALF_W, px};
    const int outerY[] = {baseY, baseY, tipY};
    renderer.fillPolygon(outerX, outerY, 3, true);
    const int innerX[] = {px - POINTER_HALF_W + 4, px + POINTER_HALF_W - 4, px};
    const int innerY[] = {baseY, baseY, tipY - dir * 5};
    renderer.fillPolygon(innerX, innerY, 3, false);
  }
  bool firstSegment = true;
  const int lineH = renderer.getLineHeight(BAR_FONT_ID);
  for (int i = 0; i < ACTION_COUNT; ++i) {
    const Box& seg = layout.actions[i];
    if (seg.w <= 0) continue;
    if (!firstSegment) {
      renderer.fillRect(seg.x - BORDER / 2, seg.y + DIVIDER_INSET, BORDER, seg.h - 2 * DIVIDER_INSET, true);
    }
    firstSegment = false;
    const char* label = selectActionLabel(i);
    const int labelW = renderer.getTextWidth(BAR_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(BAR_FONT_ID, seg.x + (seg.w - labelW) / 2, seg.y + (seg.h - lineH) / 2, label, true,
                      EpdFontFamily::BOLD);
  }
}

void ClipSelectionActivity::runSelectAction(const int action) {
  if (action == ACTION_LOOKUP) {
    if (lookupAvailable) finishLookup();
    return;
  }
  finishSelection();  // not lookupMode: builds and returns the clipping
}

bool ClipSelectionActivity::handleSelectTouch() {
  const int page = wordList.words[cursorIdx].pageIdx;
  int lx, ly;

  if (selectDrag != SelectDrag::None) {
    if (mappedInput.heldTouchPoint(lx, ly)) {
      const int idx = clipword::findWordAt(wordList, page, lx + dragOffsetX, ly + dragOffsetY);
      if (idx >= 0 && idx != cursorIdx) {
        cursorIdx = idx;
        requestUpdate();
      }
      return true;
    }
    // Lifted (or a second finger / the action bar took over): the range stands.
    mappedInput.suppressTouchContact();
    selectDrag = SelectDrag::None;
    requestUpdate();
    return true;
  }

  if (mappedInput.wasTouchDownPoint(lx, ly)) {
    const SelectLayout layout = selectLayout();
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    const bool onStart = layout.start.hit.contains(lx, ly);
    if (onStart || layout.end.hit.contains(lx, ly)) {
      // The dragged end becomes the cursor, the other end the anchor.
      selectDrag = onStart ? SelectDrag::Start : SelectDrag::End;
      startMarkIdx = onStart ? to : from;
      cursorIdx = onStart ? from : to;
      const Box word = wordBox(cursorIdx);
      dragOffsetX = word.x + word.w / 2 - lx;
      dragOffsetY = word.y + word.h / 2 - ly;
      pressedAction = -1;
      requestUpdate();  // hides the toolbar while dragging
      return true;
    }
    pressedAction = -1;
    for (int i = 0; i < ACTION_COUNT; ++i) {
      if (layout.actions[i].contains(lx, ly)) pressedAction = i;
    }
    return true;
  }

  // Hold another word: start a fresh selection there, with the same finger
  // free to drag it out (handleAdoptedContact takes the contact over).
  if (mappedInput.wasTouchLongPressPoint(lx, ly)) {
    const SelectLayout layout = selectLayout();
    const bool onOverlay =
        layout.bar.contains(lx, ly) || layout.start.hit.contains(lx, ly) || layout.end.hit.contains(lx, ly);
    const int idx = onOverlay ? -1 : clipword::findWordAt(wordList, page, lx, ly);
    if (idx >= 0) {
      startMarkIdx = cursorIdx = idx;
      contactAdopted = true;
      touchWasHeld = true;
      contactStartWord = idx;
      dragging = false;
      pressedAction = -1;
      requestUpdate();
    } else {
      mappedInput.suppressTouchContact();
    }
    return true;
  }

  if (mappedInput.wasTapPoint(lx, ly)) {
    const SelectLayout layout = selectLayout();
    const int pressed = pressedAction;
    pressedAction = -1;
    for (int i = 0; i < ACTION_COUNT; ++i) {
      if (layout.actions[i].contains(lx, ly)) {
        if (i == pressed) runSelectAction(i);
        return true;
      }
    }
    if (layout.bar.contains(lx, ly) || layout.start.hit.contains(lx, ly) || layout.end.hit.contains(lx, ly)) {
      return true;
    }
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; ++i) {
      if (wordBox(i).contains(lx, ly)) return true;  // a tap on the selection keeps it
    }
    // Anywhere else: done with the selection, back to the page.
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
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
#if FREEINK_DEVICE_X4PRO
  // The toolbar is hidden while a finger is dragging either end of the range.
  if (selectMode) drawSelectOverlay(selectLayout(), !contactAdopted && selectDrag == SelectDrag::None);
#endif

  const auto confirmLabel = selectMode   ? tr(STR_CLIP)
                            : lookupMode ? tr(STR_LOOKUP)
                                         : (startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE));
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

  // Select mode marks its ends with handles; an inverted cursor word on top of
  // the range would read as a second, different selection.
  if (!selectMode && words[cursorIdx].pageIdx == currentDisplayPage) {
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
