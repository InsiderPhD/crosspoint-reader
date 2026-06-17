#pragma once

#include <cstdint>
#include <string>

class GfxRenderer;
class MappedInputManager;
class ButtonNavigator;

// Long-press-Confirm book options modal, shared between any activity that
// surfaces book covers (HomeActivity recents tile, LibraryActivity grid).
// Manages all three pieces of state needed:
//   - long-press trigger (hold Confirm >= 700ms)
//   - "awaiting release" gate (don't accept menu input until the triggering hold ends)
//   - modal navigation + dismissal
// Host wires four things: (1) call checkLongPress when selector is on a book,
// (2) call handleInput when isOpen(), (3) call render() during render pass,
// (4) dispatch the returned Action.
class BookContextMenu {
 public:
  // Action ordering matches the legacy in-HomeActivity layout so the visual
  // option list is unchanged.
  enum class Action : uint8_t {
    MarkRead = 0,
    ResetProgress = 1,
    Shelve = 2,
    Delete = 3,
    Reindex = 4,
    RegenerateCover = 5,
    BookInfo = 6,
  };
  static constexpr int OPTIONS_COUNT = 7;
  static constexpr uint32_t DEFAULT_HOLD_MS = 700;

  bool isOpen() const { return showing_; }

  const std::string& path() const { return path_; }
  const std::string& title() const { return title_; }
  const std::string& author() const { return author_; }
  int progressPercent() const { return progressPercent_; }

  // Long-press detection. Call once per frame while the selector is on a real
  // book. If Confirm has been held for `holdMs` and the menu isn't already open,
  // opens the modal and returns true. Returns false otherwise. After a true
  // result the host should call requestUpdate() and stop processing input for
  // this frame.
  bool checkLongPress(const MappedInputManager& input, std::string path, std::string title, std::string author,
                      int progressPercent, uint32_t holdMs = DEFAULT_HOLD_MS);

  // Optional tags line, shown in the popup when non-empty. Set once by the host right
  // after checkLongPress() opens the menu (tags aren't always cheaply available, so
  // they're supplied separately rather than through checkLongPress).
  void setInfoTags(std::string tags) { tags_ = std::move(tags); }

  // Drives the modal while open. Up/Down (via ButtonNavigator) move the
  // selection; Confirm release after the initial-hold release selects;
  // Back release dismisses.
  // Returns true on a terminal transition (action chosen or cancelled). On
  // return: *outAction holds the chosen Action (only meaningful when
  // *outCancelled is false). The modal auto-closes on terminal transitions.
  bool handleInput(ButtonNavigator& nav, const MappedInputManager& input, Action* outAction, bool* outCancelled);

  // Draws the modal on top of the current framebuffer. Caller decides when
  // to invoke (typically at the end of render() after the underlying screen
  // content is drawn).
  void render(GfxRenderer& renderer) const;

  // Force-close without firing an action — e.g. when host transitions away.
  void close();

  // True if the most recently-finished press cycle resulted in a long-press
  // trigger. Used by host's normal short-press handler to suppress the
  // "open book" action on the release that originated the long-press.
  // Consumed on call (one-shot).
  bool consumeLongPressFlag();

 private:
  bool showing_ = false;
  bool triggered_ = false;         // Long-press fired during current hold cycle.
  bool awaitingRelease_ = false;   // True after open, until Confirm is released.
  bool longPressFlagSet_ = false;  // One-shot for short-press suppression.

  int selectedIndex_ = 0;
  std::string path_;
  std::string title_;
  std::string author_;
  std::string tags_;
  int progressPercent_ = -1;
};
