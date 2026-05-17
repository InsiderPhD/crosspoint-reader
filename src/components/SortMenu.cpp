#include "SortMenu.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

bool SortMenu::checkTrigger(const MappedInputManager& input, SortMode current) {
  if (showing_) return false;
  if (!input.wasReleased(MappedInputManager::Button::Power)) return false;

  showing_ = true;
  current_ = current;
  selectedIndex_ = static_cast<int>(current);
  return true;
}

bool SortMenu::handleInput(ButtonNavigator& nav, const MappedInputManager& input, SortMode* outPicked,
                           bool* outCancelled) {
  if (!showing_) return false;

  nav.onNext([this] { selectedIndex_ = (selectedIndex_ + 1) % SORT_MODE_COUNT; });
  nav.onPrevious([this] { selectedIndex_ = (selectedIndex_ - 1 + SORT_MODE_COUNT) % SORT_MODE_COUNT; });

  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (outPicked) *outPicked = static_cast<SortMode>(selectedIndex_);
    if (outCancelled) *outCancelled = false;
    showing_ = false;
    return true;
  }

  if (input.wasReleased(MappedInputManager::Button::Back)) {
    if (outCancelled) *outCancelled = true;
    showing_ = false;
    return true;
  }

  // A second Power short-press while the menu is open dismisses it without selecting,
  // mirroring how a single tap toggles a modal in most UIs.
  if (input.wasReleased(MappedInputManager::Button::Power)) {
    if (outCancelled) *outCancelled = true;
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
  constexpr int OPTIONS = SORT_MODE_COUNT;

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int POPUP_H = HEADER_H + OPTIONS * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - POPUP_H) / 2;

  renderer.fillRect(px, py, POPUP_W, POPUP_H, false);
  renderer.drawRect(px, py, POPUP_W, POPUP_H, BORDER, true);

  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + (HEADER_H - lineH) / 2, tr(STR_SORT_BY), true);
  renderer.drawLine(px + BORDER, py + HEADER_H, px + POPUP_W - BORDER - 1, py + HEADER_H);

  for (int i = 0; i < OPTIONS; i++) {
    const int optY = py + HEADER_H + i * OPTION_H;
    const bool selected = (i == selectedIndex_);
    if (selected) {
      renderer.fillRect(px + BORDER, optY, POPUP_W - BORDER * 2, OPTION_H, true);
    }
    const char* label = sortModeLabel(static_cast<SortMode>(i));
    const bool isCurrent = (static_cast<SortMode>(i) == current_);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s", isCurrent ? "\xE2\x80\xA2 " : "  ", label);
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, optY + (OPTION_H - lineH) / 2, buf, !selected);
  }
}

void SortMenu::close() {
  showing_ = false;
  selectedIndex_ = 0;
}
