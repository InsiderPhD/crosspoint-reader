#include "SortMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

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
  return true;
}

bool SortMenu::handleInput(ButtonNavigator& nav, const MappedInputManager& input, SortMode* outMode) {
  if (!showing_) return false;

  nav.onNext([this] { cursorIndex_ = (cursorIndex_ + 1) % optionCount_; });
  nav.onPrevious([this] { cursorIndex_ = (cursorIndex_ - 1 + optionCount_) % optionCount_; });

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
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
  if (input.wasReleased(MappedInputManager::Button::Back) ||
      input.wasReleased(MappedInputManager::Button::Power)) {
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
  constexpr int OPTION_H = 30;
  const int OPTIONS = optionCount_;

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int chosenIdx = optionIndexOf(chosen_);
  const int POPUP_H = HEADER_H + OPTIONS * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - POPUP_H) / 2;

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
}
