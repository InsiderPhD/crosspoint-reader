#pragma once

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

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
#else
// Non-touch boards: compiles to a constant so call sites need no #if fence.
inline TapResult tapRow(const MappedInputManager&, const Rect&, int, int, bool, int&) { return TapResult::None; }
#endif

}  // namespace TouchListNav
