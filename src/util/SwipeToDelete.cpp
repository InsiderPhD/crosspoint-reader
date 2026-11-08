#include "SwipeToDelete.h"

#if FREEINK_CAP_TOUCH

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {
constexpr int BUTTON_FONT_ID = UI_10_FONT_ID;
}  // namespace

bool SwipeToDelete::buttonRect(const GfxRenderer& renderer, const Rect& listRect, const int selectedIndex,
                               const bool hasSubtitle, Rect& out) const {
  if (row < 0) return false;
  const auto geo = GUI.listGeometry(listRect, selectedIndex, hasSubtitle);
  const int slot = row - geo.pageStart;
  if (slot < 0 || slot >= geo.pageItems) return false;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int labelW = renderer.getTextWidth(BUTTON_FONT_ID, tr(STR_DELETE), EpdFontFamily::BOLD);
  // Wide enough to hit comfortably with a fingertip even for a short label.
  int width = labelW + 2 * metrics.contentSidePadding;
  if (width < geo.rowHeight * 2) width = geo.rowHeight * 2;
  constexpr int inset = 2;  // keep clear of the row separators some themes draw
  out = Rect{listRect.x + listRect.width - metrics.contentSidePadding - width,
             listRect.y + slot * geo.rowHeight + inset, width, geo.rowHeight - 2 * inset};
  return true;
}

SwipeToDelete::Event SwipeToDelete::handle(const MappedInputManager& input, const Rect& listRect, const int itemCount,
                                           const int selectedIndex, const bool hasSubtitle, int& outRow,
                                           const RowFilter canDelete, const void* ctx) {
  using Button = MappedInputManager::Button;
  if (!CrossPointSettings::FULL_TOUCH_UI) return Event::None;

  if (swallowRelease) {
    if (input.wasReleased(Button::Back) || input.wasReleased(Button::Confirm)) {
      swallowRelease = false;
      return Event::Consumed;
    }
    if (!input.isPressed(Button::Back) && !input.isPressed(Button::Confirm)) swallowRelease = false;
  }

  int sx = 0;
  int sy = 0;
  const auto swipe = input.wasSwipe(sx, sy);

  if (row >= 0) {
    if (row >= itemCount) {
      row = -1;  // the list changed under the open button
      return Event::Consumed;
    }
    int lx, ly;
    if (input.wasTapPoint(lx, ly)) {
      // Against the button as last painted (render() owns the font metrics),
      // and only if it was painted for this row: a tap that beats the paint
      // just closes.
      const bool onButton = drawnRow == row && lx >= drawnButton.x && lx < drawnButton.x + drawnButton.width &&
                            ly >= drawnButton.y && ly < drawnButton.y + drawnButton.height;
      outRow = row;
      row = -1;
      return onButton ? Event::Delete : Event::Consumed;
    }
    if (input.wasTouchLongPressPoint(lx, ly)) {
      input.suppressTouchContact();  // its lift must not come back as a tap
      row = -1;
      return Event::Consumed;
    }
    if (input.wasPressed(Button::Back) || input.wasPressed(Button::Confirm)) {
      row = -1;
      swallowRelease = true;
      return Event::Consumed;
    }
    // A page swipe or a side key moves on as usual; the button just goes away.
    if (swipe == MappedInputManager::Swipe::Up || swipe == MappedInputManager::Swipe::Down ||
        input.wasPressed(Button::Up) || input.wasPressed(Button::Down) || input.wasPressed(Button::Left) ||
        input.wasPressed(Button::Right) || input.wasPressed(Button::PageBack) ||
        input.wasPressed(Button::PageForward)) {
      row = -1;
      return Event::None;
    }
  }

  if (swipe != MappedInputManager::Swipe::Right) return Event::None;
  const int hit = GUI.hitTestList(listRect, itemCount, selectedIndex, hasSubtitle, sx, sy);
  if (hit < 0 || (canDelete && !canDelete(ctx, hit))) {
    // A flick outside the rows (or on one that cannot go) closes an open button.
    if (row < 0) return Event::None;
    row = -1;
    return Event::Consumed;
  }
  row = static_cast<int16_t>(hit);
  return Event::Consumed;
}

void SwipeToDelete::draw(const GfxRenderer& renderer, const Rect& listRect, const int itemCount,
                         const int selectedIndex, const bool hasSubtitle) const {
  drawnRow = -1;
  if (row < 0 || row >= itemCount) return;
  Rect button;
  if (!buttonRect(renderer, listRect, selectedIndex, hasSubtitle, button)) return;
  drawnButton = button;
  drawnRow = row;
  // A white gap first, so the row's title is visibly cut rather than running
  // into the button.
  constexpr int gap = 6;
  renderer.fillRect(button.x - gap, button.y, gap, button.height, false);
  renderer.fillRect(button.x, button.y, button.width, button.height, true);
  const char* label = tr(STR_DELETE);
  const int labelW = renderer.getTextWidth(BUTTON_FONT_ID, label, EpdFontFamily::BOLD);
  const int labelY = button.y + (button.height - renderer.getLineHeight(BUTTON_FONT_ID)) / 2;
  renderer.drawText(BUTTON_FONT_ID, button.x + (button.width - labelW) / 2, labelY, label, false, EpdFontFamily::BOLD);
}

#endif
