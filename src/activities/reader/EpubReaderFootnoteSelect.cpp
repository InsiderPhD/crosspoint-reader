// Choosing a footnote on the reading page itself (EpubReaderActivity members).
//
// Upstream (#3682) and CrossInk do this in a sub-activity that owns a second copy
// of the page plus a 4KB framebuffer snapshot. Here it is a mode of the reader:
// the page is the one the reader already renders, the highlight is an in-place
// invert of the marker's pixels, and the whole state is four bytes (see
// EpubReaderActivity.h). That matters because the reader's tightest state —
// on-page footnotes with a Bluetooth remote connected — has under 10KB free.

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "FootnoteMarkers.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"

namespace {

// Same hold as the reader's own power long-press (skipChapterMs).
constexpr unsigned long POWER_SLEEP_HOLD_MS = 700;

// footnoteSelectHeld bits: buttons that were down when the selection opened.
constexpr uint8_t HELD_CONFIRM = 1 << 0;
constexpr uint8_t HELD_BACK = 1 << 1;
constexpr uint8_t HELD_POWER = 1 << 2;

struct HighlightCtx {
  const GfxRenderer& renderer;
  int fontId;
  int originX;
  int originY;
  uint8_t selected;
};

// Inverts one word of the selected marker. The glyphs are already in the
// framebuffer, so nothing is rasterized and nothing is allocated.
void invertMarkerWord(void* raw, const FootnoteMarkers::Word& word) {
  const auto& ctx = *static_cast<const HighlightCtx*>(raw);
  if (word.footnote != ctx.selected) return;

  const TextBlock& block = *word.block;
  const BlockStyle& blockStyle = block.getBlockStyle();
  const int fontId = blockStyle.fontOverride != 0 ? blockStyle.fontOverride : ctx.fontId;
  // The advance the layout placed the word with (already halved for SUP/SUB).
  const auto style = static_cast<EpdFontFamily::Style>(block.wordStyle(word.index) & ~EpdFontFamily::UNDERLINE);
  const int width =
      ctx.renderer.getTextAdvanceX(fontId, block.wordText(word.index), style, blockStyle.characterSpacing);
  if (width <= 0) return;

  // Two pixels of margin each side so a lone superscript digit still reads as a
  // block. The right margin stops at the next word: a marker glued to its
  // neighbour ("...text12,") would otherwise flip a column of a glyph that is not
  // part of it. The word's own advance is always covered.
  const int wordLeft = ctx.originX + word.x;
  const int left = wordLeft - 2;
  int right = wordLeft + width + 2;
  if (word.index + 1 < block.wordCount() && block.wordXpos(word.index + 1) > block.wordXpos(word.index)) {
    const int nextLeft = wordLeft + block.wordXpos(word.index + 1) - block.wordXpos(word.index);
    right = std::min(right, std::max(nextLeft, wordLeft + width));
  }
  // Line top down to just past the link underline (drawn at ascender + 2).
  ctx.renderer.invertRect(left, ctx.originY + word.y, right - left, ctx.renderer.getFontAscenderSize(fontId) + 4);
}

struct HitCtx {
  int x;  // tap, in page coordinates
  int y;
  int lineHeight;
  int8_t hit;
};

// Finger-sized hit test. Deliberately font-free (runs on the main task, outside
// the render's glyph prewarm): a word's box runs to the next word's left edge,
// or one line height when it ends its line, plus a margin a fingertip needs.
void hitTestMarkerWord(void* raw, const FootnoteMarkers::Word& word) {
  constexpr int SLOP = 14;
  auto& ctx = *static_cast<HitCtx*>(raw);
  if (ctx.hit >= 0) return;

  const TextBlock& block = *word.block;
  int width = ctx.lineHeight;
  if (word.index + 1 < block.wordCount() && block.wordXpos(word.index + 1) > block.wordXpos(word.index)) {
    width = block.wordXpos(word.index + 1) - block.wordXpos(word.index);
  }
  if (ctx.x >= word.x - SLOP && ctx.x < word.x + width + SLOP && ctx.y >= word.y - SLOP &&
      ctx.y < word.y + ctx.lineHeight + SLOP) {
    ctx.hit = static_cast<int8_t>(word.footnote);
  }
}

}  // namespace

void EpubReaderActivity::openFootnoteList() {
  startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             navigateToHref(std::get<FootnoteResult>(result.data).href, true);
                           }
                           requestUpdate();
                         });
}

void EpubReaderActivity::openFootnoteSelect() {
  std::string onlyHref;
  bool useList = false;
  {
    // The render task swaps currentPageFootnotes out and back around a render.
    RenderLock lock(*this);
    if (!section || currentPageFootnotes.empty()) return;

    if (currentPageFootnotes.size() == 1) {
      // Nothing to choose between.
      onlyHref = currentPageFootnotes[0].href;
    } else {
      // Select on the page only when every entry has a marker here, so nothing the
      // list offers becomes unreachable. The entries without one are the tail of a
      // note carried over from the previous page and notes the layout had to defer
      // off their reference's page; the two cannot be told apart from what a page
      // stores, and for the second kind the list is the only way to the note.
      const uint16_t mask = footnoteMarkerMask;
      const uint16_t all = static_cast<uint16_t>((1U << currentPageFootnotes.size()) - 1U);
      useList = mask != all;
      if (!useList) {
        footnoteSelectIndex = static_cast<int8_t>(__builtin_ctz(mask));
      }
      LOG_DBG("ERS", "Footnote select: %u notes, markers 0x%04x -> %s",
              static_cast<unsigned>(currentPageFootnotes.size()), static_cast<unsigned>(mask),
              useList ? "list" : "on page");
    }
  }

  if (!onlyHref.empty()) {
    navigateToHref(onlyHref, true);
    requestUpdate();
    return;
  }
  if (useList) {
    openFootnoteList();
    return;
  }

  // A hold-bound entry (long-press Confirm, say) fires with the button still
  // down; its release must not be read as "open the highlighted footnote".
  using Button = MappedInputManager::Button;
  footnoteSelectHeld = static_cast<uint8_t>((mappedInput.isPressed(Button::Confirm) ? HELD_CONFIRM : 0) |
                                            (mappedInput.isPressed(Button::Back) ? HELD_BACK : 0) |
                                            (mappedInput.isPressed(Button::Power) ? HELD_POWER : 0));
  requestUpdate();
}

void EpubReaderActivity::cancelFootnoteSelect() {
  footnoteSelectIndex = -1;
  // An automatic page turn was paused for the selection; give the page a full
  // interval again rather than turning the moment the highlight goes away.
  lastPageTurnTime = millis();
  requestUpdate();
}

void EpubReaderActivity::followSelectedFootnote() {
  std::string href;
  {
    RenderLock lock(*this);
    const int8_t index = footnoteSelectIndex;
    if (index >= 0 && static_cast<size_t>(index) < currentPageFootnotes.size()) {
      href = currentPageFootnotes[index].href;
    }
  }
  footnoteSelectIndex = -1;
  lastPageTurnTime = millis();
  if (!href.empty()) navigateToHref(href, true);
  // Also covers an href that did not resolve: the highlight has to come off.
  requestUpdate();
}

void EpubReaderActivity::stepFootnoteSelect(const int direction) {
  const uint16_t mask = footnoteMarkerMask;
  int index = footnoteSelectIndex;
  if (mask == 0 || index < 0) return;
  for (int n = 0; n < 16; ++n) {
    index = (index + direction + 16) % 16;
    if ((mask >> index) & 1U) break;
  }
  if (index == footnoteSelectIndex) return;
  footnoteSelectIndex = static_cast<int8_t>(index);
  requestUpdate();
}

int8_t EpubReaderActivity::footnoteMarkerAt(const int x, const int y) {
  RenderLock lock(*this);
  if (!section) return -1;
  // Touch boards only (X4 Pro, which has PSRAM): the page is loaded for the
  // length of this call to find what is under the finger, then dropped.
  const auto page = section->loadPageFromSectionFile();
  if (!page) return -1;

  int marginTop, marginRight, marginBottom, marginLeft;
  computeOrientedMargins(marginTop, marginRight, marginBottom, marginLeft);
  HitCtx ctx{x - marginLeft, y - marginTop, renderer.getLineHeight(SETTINGS.getReaderFontId()), -1};
  FootnoteMarkers::scan(*page, &hitTestMarkerWord, &ctx);
  return ctx.hit;
}

bool EpubReaderActivity::handleFootnoteSelectInput() {
  if (footnoteSelectIndex < 0) return false;
  using Button = MappedInputManager::Button;

  // Sleep stays where it always is.
  if (mappedInput.isPressed(Button::Power) && mappedInput.getHeldTime(Button::Power) >= POWER_SLEEP_HOLD_MS) {
    footnoteSelectIndex = -1;
    enterDeepSleepFromReaderAction();
    return true;
  }

  // Keep the chord tracker fed so a combo that opened the selection still gets
  // its release frames swallowed; a combo completed in here does nothing.
  if (readerCombos.update(mappedInput) != ReaderCombos::Result::None) return true;

  // True when this frame's release of `button` belongs to the press that opened
  // the selection.
  const auto spentRelease = [this](const Button button, const uint8_t bit) {
    if ((footnoteSelectHeld & bit) == 0) return false;
    const bool released = mappedInput.wasReleased(button);
    if (released || !mappedInput.isPressed(button)) footnoteSelectHeld &= static_cast<uint8_t>(~bit);
    return released;
  };
  const bool confirmSpent = spentRelease(Button::Confirm, HELD_CONFIRM);
  const bool backSpent = spentRelease(Button::Back, HELD_BACK);
  const bool powerSpent = spentRelease(Button::Power, HELD_POWER);

  if (!backSpent && mappedInput.wasReleased(Button::Back)) {
    cancelFootnoteSelect();
    return true;
  }
  // Power opens it too (upstream #3766), unless this release is the tail of the
  // Power+Down screenshot combo.
  if ((!confirmSpent && mappedInput.wasReleased(Button::Confirm)) ||
      (!powerSpent && mappedInput.wasReleased(Button::Power) && !gpio.wasReleased(HalGPIO::BTN_DOWN))) {
    followSelectedFootnote();
    return true;
  }

  if (mappedInput.hasTouch()) {
    int tapX, tapY;
    if (mappedInput.wasScreenTapped(tapX, tapY)) {
      const int8_t hit = footnoteMarkerAt(tapX, tapY);
      if (hit >= 0) {
        footnoteSelectIndex = hit;
        followSelectedFootnote();
      } else {
        cancelFootnoteSelect();  // a tap on anything else puts the page back
      }
      return true;
    }
#if FREEINK_DEVICE_X4PRO
    if (mappedInput.homeKeyGesture() != MappedInputManager::HomeKeyGesture::None) {
      cancelFootnoteSelect();
      return true;
    }
#endif
  }

  // Every directional control steps, the side keys (and so a page-turn remote)
  // included: while the highlight is up they choose a footnote, not a page.
#if FREEINK_DEVICE_X4PRO
  constexpr bool swapFront = false;
#else
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
#endif
  const bool frontPrev = mappedInput.wasPressed(swapFront ? Button::Right : Button::Left);
  const bool frontNext = mappedInput.wasPressed(swapFront ? Button::Left : Button::Right);
  if (frontPrev || mappedInput.wasPressed(Button::PageBack)) {
    stepFootnoteSelect(-1);
  } else if (frontNext || mappedInput.wasPressed(Button::PageForward)) {
    stepFootnoteSelect(1);
  }
  return true;
}

void EpubReaderActivity::drawFootnoteSelection(const Page& page, const int marginLeft, const int marginTop) const {
  const int8_t index = footnoteSelectIndex;
  if (index < 0) return;
  HighlightCtx ctx{renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, static_cast<uint8_t>(index)};
  FootnoteMarkers::scan(page, &invertMarkerWord, &ctx);
}
