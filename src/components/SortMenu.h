#pragma once

#include <cstdint>

#include "sorting/SortMode.h"

class GfxRenderer;
class MappedInputManager;
class ButtonNavigator;

// Power-button sort-picker modal, shared by activities that show file/book lists.
// Opens on a short-press release of the Power button. Each row is a sort *field*; the current
// direction is shown inline in the row label (e.g. "Author A-Z" ↔ "Author Z-A"):
//   Up/Down  — move the field cursor
//   Confirm  — select the cursor's field; pressing Confirm again on the active field flips
//              its direction (primary ↔ reverse). Never closes the menu.
//   Back     — close and commit the chosen field/direction (a second Power short-press too).
// There is no separate cancel: `chosen` only ever changes on an explicit Confirm, so closing
// commits exactly what the user selected (and closing without selecting is a no-op).
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
  SortMode current() const { return chosen_; }

  // Detect Power short-press. Pass the active sort (to preselect its field/direction) and the
  // list of fields this context offers (`options`/`count`). `options` must outlive the menu
  // (pass a static constexpr array). Returns true on the frame the menu opens; host should
  // requestUpdate() + skip other input.
  bool checkTrigger(const MappedInputManager& input, SortMode current, const SortOption* options, int count);

  // Drive the modal. Up/Down move the field cursor; Confirm selects/flips (see class comment);
  // Back or a second Power short-press closes. Returns true only on close, writing the chosen
  // mode to *outMode. While it returns false the host should requestUpdate() to reflect the
  // cursor/label change.
  bool handleInput(ButtonNavigator& nav, const MappedInputManager& input, SortMode* outMode);

  void render(GfxRenderer& renderer) const;

  void close();

 private:
  // Index of the option whose primary or reverse equals `m`, or -1 if none.
  int optionIndexOf(SortMode m) const;

  bool showing_ = false;
  int cursorIndex_ = 0;                        // highlighted field row
  SortMode chosen_ = SortMode::AlphabeticAsc;  // staged selection; committed on close
  // Context-supplied field list (not owned; must outlive the menu). Set by checkTrigger.
  const SortOption* options_ = ALL_SORT_OPTIONS;
  int optionCount_ = ALL_SORT_OPTIONS_COUNT;
};
