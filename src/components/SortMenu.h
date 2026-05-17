#pragma once

#include <cstdint>

#include "sorting/SortMode.h"

class GfxRenderer;
class MappedInputManager;
class ButtonNavigator;

// Power-button sort-picker modal, shared by activities that show file/book lists.
// Opens on a short-press release of the Power button. Up/Down navigate the option list;
// Confirm picks, Back cancels, a second Power short-press also dismisses.
//
// Hosts wire three things: (1) call checkTrigger every frame, (2) call handleInput while
// isOpen(), (3) call render() during render pass. The host's `if (sortMenu.isOpen()) { …;
// return; }` block guarantees the release that closes the menu can't also fire the
// activity's normal Confirm/Back handler in the same frame — no extra suppression needed.
//
// Scope note: in the reader, Power short-press is controlled by `SETTINGS.shortPwrBtn`
// (PAGE_TURN by default). This menu only applies in non-reader list activities.
class SortMenu {
 public:
  bool isOpen() const { return showing_; }
  SortMode current() const { return current_; }

  // Detect Power short-press. Pass the current sort mode so the menu can highlight it.
  // Returns true on the frame the menu opens; host should requestUpdate() + skip other input.
  bool checkTrigger(const MappedInputManager& input, SortMode current);

  // Drive the modal. Up/Down (via ButtonNavigator) move selection; Confirm release picks;
  // Back release dismisses. Returns true on terminal transition.
  bool handleInput(ButtonNavigator& nav, const MappedInputManager& input, SortMode* outPicked, bool* outCancelled);

  void render(GfxRenderer& renderer) const;

  void close();

 private:
  bool showing_ = false;
  int selectedIndex_ = 0;
  SortMode current_ = SortMode::AlphabeticAsc;
};
