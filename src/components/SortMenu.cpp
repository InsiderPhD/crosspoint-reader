#include "SortMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

int SortMenu::optionIndexOf(SortMode m) const {
  for (int i = 0; i < optionCount_; i++) {
    if (options_[i].primary == m || options_[i].reverse == m) return i;
  }
  return -1;
}

bool SortMenu::checkTrigger(const MappedInputManager& input, SortMode current, const SortOption* options, int count) {
  if (showing_) return false;
  if (!input.wasReleased(MappedInputManager::Button::Power)) return false;
  if (!options || count <= 0) return false;

  showing_ = true;
  chosen_ = current;
  options_ = options;
  optionCount_ = count;
  // Open with the cursor on the active field when it's offered here; otherwise start at the top.
  const int idx = optionIndexOf(current);
  cursorIndex_ = (idx >= 0) ? idx : 0;
#if FREEINK_DEVICE_X4PRO
  popupRectValid_ = false;  // stale from a previous open until render() runs
  dismissOnNextConfirm_ = false;
  swallowNextConfirm_ = false;
#endif
  return true;
}

bool SortMenu::handleInput(ButtonNavigator& nav, const MappedInputManager& input, SortMode* outMode) {
  if (!showing_) return false;

  nav.onNext([this] { cursorIndex_ = (cursorIndex_ + 1) % optionCount_; });
  nav.onPrevious([this] { cursorIndex_ = (cursorIndex_ - 1 + optionCount_) % optionCount_; });

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a tap outside the popup closes the menu (committing the staged
  // choice, same as Back); a tap on an unselected field row moves the cursor
  // there (two-tap); a tap on the cursor's row stays with the injected Confirm
  // below (select/flip that field). The redirects act on the injected Confirm
  // release this tap generates — see the latches in the header.
  if (SETTINGS.fullTouchUi && popupRectValid_) {
    int tapX, tapY;
    if (input.wasTapPoint(tapX, tapY)) {
      const bool insidePopup =
          tapX >= popupX_ && tapX < popupX_ + popupW_ && tapY >= popupY_ && tapY < popupY_ + popupH_;
      if (!insidePopup) {
        dismissOnNextConfirm_ = true;
      } else {
        const int row = (tapY - optionsTopY_) / OPTION_ROW_H;
        const bool onRow = tapY >= optionsTopY_ && row >= 0 && row < optionCount_;
        if (onRow && row != cursorIndex_) {
          cursorIndex_ = row;
          swallowNextConfirm_ = true;
        } else if (!onRow) {
          swallowNextConfirm_ = true;  // header tap: no action
        }
      }
    }
  }
#endif

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
#if FREEINK_DEVICE_X4PRO
    if (dismissOnNextConfirm_) {
      dismissOnNextConfirm_ = false;
      if (outMode) *outMode = chosen_;
      showing_ = false;
      return true;
    }
    if (swallowNextConfirm_) {
      swallowNextConfirm_ = false;
      return false;  // cursor move already happened; host redraws
    }
#endif
    const SortOption& opt = options_[cursorIndex_];
    const bool hasReverse = (opt.reverse != opt.primary);
    if (hasReverse && chosen_ == opt.primary) {
      chosen_ = opt.reverse;  // active field → flip to reverse
    } else if (hasReverse && chosen_ == opt.reverse) {
      chosen_ = opt.primary;  // active field → flip back
    } else {
      chosen_ = opt.primary;  // newly selected field → primary direction
    }
    return false;  // stay open; host redraws the updated label
  }

  // Back — or a second Power short-press — closes and commits the staged choice.
  if (input.wasReleased(MappedInputManager::Button::Back) || input.wasReleased(MappedInputManager::Button::Power)) {
    if (outMode) *outMode = chosen_;
    showing_ = false;
    return true;
  }

  return false;
}

void SortMenu::render(GfxRenderer& renderer) const {
  if (!showing_) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int POPUP_W = 380;
  constexpr int BORDER = 2;
  constexpr int H_PAD = 14;
  constexpr int HEADER_H = 34;
#if FREEINK_DEVICE_X4PRO
  constexpr int OPTION_H = OPTION_ROW_H;  // shared with handleInput's tap-row mapping
#else
  constexpr int OPTION_H = 30;
#endif
  const int OPTIONS = optionCount_;

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int chosenIdx = optionIndexOf(chosen_);
  const int POPUP_H = HEADER_H + OPTIONS * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - POPUP_H) / 2;

#if FREEINK_DEVICE_X4PRO
  // Cache the popup bounds for handleInput's tap handling.
  popupX_ = px;
  popupY_ = py;
  popupW_ = POPUP_W;
  popupH_ = POPUP_H;
  optionsTopY_ = py + HEADER_H;
  popupRectValid_ = true;
#endif

  renderer.fillRect(px, py, POPUP_W, POPUP_H, false);
  renderer.drawRect(px, py, POPUP_W, POPUP_H, BORDER, true);

  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + (HEADER_H - lineH) / 2, tr(STR_SORT_BY), true);
  renderer.drawLine(px + BORDER, py + HEADER_H, px + POPUP_W - BORDER - 1, py + HEADER_H);

  for (int i = 0; i < OPTIONS; i++) {
    const int optY = py + HEADER_H + i * OPTION_H;
    const bool selected = (i == cursorIndex_);
    if (selected) {
      renderer.fillRect(px + BORDER, optY, POPUP_W - BORDER * 2, OPTION_H, true);
    }
    // The chosen field shows its active direction (e.g. "Author Z-A"); other fields show
    // their primary/default direction. A bullet marks the chosen field.
    const bool isChosen = (i == chosenIdx);
    const char* label = sortModeLabel(isChosen ? chosen_ : options_[i].primary);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", isChosen ? "\xE2\x80\xA2 " : "  ", label);
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, optY + (OPTION_H - lineH) / 2, buf, !selected);
  }
}

void SortMenu::close() {
  showing_ = false;
  cursorIndex_ = 0;
#if FREEINK_DEVICE_X4PRO
  popupRectValid_ = false;
  dismissOnNextConfirm_ = false;
  swallowNextConfirm_ = false;
#endif
}
