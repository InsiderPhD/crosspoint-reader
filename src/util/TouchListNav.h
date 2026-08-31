#pragma once

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

// Full Touch (X4 Pro) tap dispatch for the standard drawList screens.
// Two-tap model: a tap on an unselected row only moves the cursor there; a
// second tap on the now-selected row activates it. Usage in an activity's
// loop(), before the Confirm handling:
//
//   int tappedIndex;
//   switch (TouchListNav::tapRow(mappedInput, listRect(), itemCount, selectedIndex,
//                                /*hasSubtitle=*/false, tappedIndex)) {
//     case TouchListNav::TapResult::SelectionMoved:
//       selectedIndex = tappedIndex;
//       requestUpdate();
//       return;
//     case TouchListNav::TapResult::Activated:
//       handleSelection();  // the same function the Confirm branch calls
//       return;
//     case TouchListNav::TapResult::None:
//       break;
//   }
//
// The activity must also override handlesDirectTouch() so the main loop stops
// injecting Confirm for taps, and rect/selectedIndex/hasSubtitle must match its
// drawList call (they determine the visible page and row height).
namespace TouchListNav {

enum class TapResult { None, SelectionMoved, Activated };

#if FREEINK_DEVICE_X4PRO
// A tap on dead space (header, gaps, below the last row) returns None and is
// dropped — gestures still work there.
inline TapResult tapRow(const MappedInputManager& mappedInput, const Rect& rect, int itemCount, int selectedIndex,
                        bool hasSubtitle, int& outIndex) {
  if (!SETTINGS.fullTouchUi) {
    return TapResult::None;
  }
  int lx, ly;
  if (!mappedInput.wasTapPoint(lx, ly)) {
    return TapResult::None;
  }
  const int index = GUI.hitTestList(rect, itemCount, selectedIndex, hasSubtitle, lx, ly);
  if (index < 0) {
    return TapResult::None;
  }
  outIndex = index;
  return index == selectedIndex ? TapResult::Activated : TapResult::SelectionMoved;
}

// Full Touch page paging for any list that actually paginates: a vertical swipe
// jumps a whole page, with the SAME sense as dragging content on a phone —
// swipe UP pulls the list up, revealing what is below it, so it goes to the
// NEXT page; swipe down goes back. (This was the other way round until
// 2026-08-30; the user asked for the iPhone convention. It is the direction of
// the CONTENT, not of the cursor or the scrollbar thumb.) Same wrap-around the
// held side key gives. Returns true when index moved, so the call site only has
// to requestUpdate() (and return, since the gesture is consumed):
//
//   if (TouchListNav::pageSwipe(mappedInput, totalItems, pageItems, selectorIndex)) {
//     requestUpdate();
//     return;
//   }
//
// Place it next to the activity's onNext/PreviousContinuous page bindings so it
// inherits whatever state gating those already sit behind. Inert unless the
// list has more items than fit on a page: on a single-page list there is no
// page to turn, and a sloppy tap the panel classifies as a swipe must not move
// the cursor invisibly. Off X4 Pro, and in gesture mode where the swipe is
// injected as a row step instead, this never fires.
inline bool pageSwipe(const MappedInputManager& mappedInput, const int itemCount, const int pageItems, int& index) {
  if (pageItems <= 0 || itemCount <= pageItems) {
    return false;
  }
  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::Swipe::Up:
      index = ButtonNavigator::nextPageIndex(index, itemCount, pageItems);
      return true;
    case MappedInputManager::Swipe::Down:
      index = ButtonNavigator::previousPageIndex(index, itemCount, pageItems);
      return true;
    default:
      return false;
  }
}

// Raw Full Touch vertical-swipe direction, for every scrollable surface that
// pageSwipe() cannot drive itself:
//   * pagination that is server-side (BookFusion browse) — one fetched page
//     fills the screen, so there is no local index to jump;
//   * continuous scroll rather than paging (the stats tables and charts, a book
//     description) — the call site owns the step size and the clamp.
// Returns +1 for swipe up (forward: reveal what is below), -1 for swipe down
// (back), 0 otherwise — the same content-drag direction as pageSwipe().
//
// The call site MUST supply its own bounds, i.e. be a no-op when there is
// nothing in that direction. Unlike pageSwipe() there is no inertness guard
// here, and a sloppy tap the panel classifies as a swipe must not fire a
// network fetch or nudge a surface that cannot move.
inline int pageSwipeDelta(const MappedInputManager& mappedInput) {
  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::Swipe::Up:
      return +1;
    case MappedInputManager::Swipe::Down:
      return -1;
    default:
      return 0;
  }
}

// Full Touch tab paging for the tabbed screens (Settings, Reading Stats): a
// rightward swipe moves to the next tab, wrapping at the end the same way
// Confirm-on-the-ribbon and the held side key already do.
//
// The backward direction is deliberately NOT here. A leftward swipe is still
// injected as Back — the X4 Pro has no physical Back button, so that gesture
// cannot be repurposed wholesale, and since 2026-08-30 Back on these screens
// simply CLOSES them in Full Touch (it used to step back a tab and close only
// from the first, which made leaving take as many gestures as there are tabs).
// A previous tab is reached by tapping it on the ribbon, or by wrapping forward
// with this swipe. Gesture mode and the button boards are unaffected: there
// Back keeps its row -> ribbon -> close ladder.
inline bool tabSwipeNext(const MappedInputManager& mappedInput) {
  return mappedInput.wasSwipe() == MappedInputManager::Swipe::Right;
}
#else
// Non-touch boards: compiles to a constant so call sites need no #if fence.
inline TapResult tapRow(const MappedInputManager&, const Rect&, int, int, bool, int&) { return TapResult::None; }
inline bool pageSwipe(const MappedInputManager&, int, int, int&) { return false; }
inline int pageSwipeDelta(const MappedInputManager&) { return 0; }
inline bool tabSwipeNext(const MappedInputManager&) { return false; }
#endif

}  // namespace TouchListNav
