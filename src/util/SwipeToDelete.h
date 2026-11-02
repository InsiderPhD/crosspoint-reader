#pragma once

#include <cstdint>

#include "MappedInputManager.h"
#include "components/UITheme.h"

class GfxRenderer;

// X4 Pro swipe-to-delete for drawList screens (upstream #3712, as adapted by
// CrossInk): a leftward flick on a row slides a Delete button out at its right
// end; tapping that button deletes the row (the caller asks for confirmation).
// Anything else closes the button again without acting: a tap elsewhere, Back
// (the rightward Back swipe or the action bar), Confirm, a long press, a side
// key, or a vertical page swipe -- the last two are then handled as usual, so
// paging a list is never lost to an open button.
//
// The leftward flick is the swipe MappedInputManager records as Swipe::Right
// (the calibrated axis runs opposite the logical one, see its mapping table).
// No list screen used it; the Back swipe is the other direction and is
// untouched. Only the screen's own loop() reads it, so a screen without this
// helper behaves exactly as before.
//
// Usage, in loop() after any modal and before the row tap handling:
//
//   int row;
//   switch (swipeDelete.handle(mappedInput, listRect(), count, selected, false, row)) {
//     case SwipeToDelete::Event::Delete:   selected = row; askToDelete(); return;
//     case SwipeToDelete::Event::Consumed: requestUpdate(); return;
//     case SwipeToDelete::Event::None:     break;
//   }
//
// and in render(), right after the drawList call with the same arguments:
//
//   swipeDelete.draw(renderer, listRect(), count, selected, false);
//
// A dozen bytes of state; no allocation. Inert (constant-folded) off the X4 Pro.
class SwipeToDelete {
 public:
  enum class Event : uint8_t { None, Consumed, Delete };
  // Rows a screen does not want deletable (folders, a parent entry...).
  using RowFilter = bool (*)(const void* ctx, int row);

#if FREEINK_DEVICE_X4PRO
  Event handle(const MappedInputManager& input, const Rect& listRect, int itemCount, int selectedIndex,
               bool hasSubtitle, int& outRow, RowFilter canDelete = nullptr, const void* ctx = nullptr);
  void draw(const GfxRenderer& renderer, const Rect& listRect, int itemCount, int selectedIndex,
            bool hasSubtitle) const;
  bool isOpen() const { return row >= 0; }
  void close() {
    row = -1;
    swallowRelease = false;
  }

 private:
  int16_t row = -1;
  // Back/Confirm closed the button on its press; its release must not then act.
  bool swallowRelease = false;
  // Written by draw() on the render task, read by handle()'s tap test: the
  // same paint/hit-test hand-off the hand-drawn popups use.
  mutable int16_t drawnRow = -1;
  mutable Rect drawnButton{};
  // The Delete button for the open row, or false when that row is off screen.
  bool buttonRect(const GfxRenderer& renderer, const Rect& listRect, int selectedIndex, bool hasSubtitle,
                  Rect& out) const;
#else
  Event handle(const MappedInputManager&, const Rect&, int, int, bool, int&, RowFilter = nullptr,
               const void* = nullptr) {
    return Event::None;
  }
  void draw(const GfxRenderer&, const Rect&, int, int, bool) const {}
  bool isOpen() const { return false; }
  void close() {}
#endif
};
