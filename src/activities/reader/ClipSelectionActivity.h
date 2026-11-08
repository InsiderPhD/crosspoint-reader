#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "WordRef.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Visual style applied to a word during selection (cursor vs. selected-range).
struct ClipWordStyle {
  enum Flags : uint8_t {
    NONE = 0,
    FILL = 1 << 0,
    INVERT = 1 << 1,
    UNDERLINE = 1 << 2,
    BORDER = 1 << 3,
  };

  uint8_t flags = FILL;
  Color fillColor = Color::LightGray;
};

// Modal word-selection overlay: Left/Right move the cursor by word, Up/Down by line,
// Confirm is a two-stage anchor->finish, Back clears the anchor or cancels. On finish it
// returns a ClippingResult built by ClipTextBuilder. Re-renders the page and overlays
// highlights each frame; deliberately does NOT snapshot the framebuffer — a second 48KB
// buffer would OOM alongside the reader's resident section/font caches on the ESP32-C3.
class ClipSelectionActivity final : public Activity {
 public:
  // initialCursorIdx/preAnchored implement the Kindle-style entry: a tap-and-hold on a
  // word in the reader opens this overlay with that word already under the cursor and
  // anchored, so the selection grows from where the finger landed.
  // lookupMode is the reader's dictionary lookup: it returns a WordPickResult
  // (the text to look up) instead of a ClippingResult, with none of the
  // surrounding context ClipTextBuilder assembles. One Confirm press looks up
  // the word under the cursor, so the common single-word case stays one press.
  // A phrase is the less common case and takes one extra gesture: hold Confirm
  // to anchor the first word, move to the last, press Confirm (on touch: drag
  // from the highlighted word to the last one). Everything else (cursor
  // movement, rendering, the range highlight) is deliberately shared, so there
  // is only ever one word selector on the page to keep working.
  // adoptContact (X4 Pro, the reader's Hold Action modes): the finger that held
  // initialCursorIdx in the reader is still down and was deliberately NOT
  // suppressed, so this screen takes that contact over as if it had landed
  // here: dragging it off the word extends the selection, and lifting it
  // finishes (lookup: looks the word or phrase up; clip: saves a dragged range,
  // or leaves the held word anchored when the finger never left it). The
  // contact is never read as a tap or a hold of this screen's own.
  // selectMode (X4 Pro, Hold Action = Select Text; CrossInk's touch text
  // selection): the held word opens selected, a drag of the adopted contact
  // extends it, and lifting leaves the range on screen with a drag handle at
  // each end and a floating toolbar -- Look Up (only when lookupAvailable) and
  // Clip. Dragging a handle moves that end; a tap on the toolbar acts; a tap
  // off the selection cancels, and holding another word starts over there.
  // Lookup returns a WordPickResult, Clip a ClippingResult, so the caller
  // handles both. Built on the visible page only (no cross-page handles).
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WordList wordList, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft,
                        int initialCursorIdx = 0, bool preAnchored = false, bool lookupMode = false,
                        bool adoptContact = false, bool selectMode = false, bool lookupAvailable = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // Taps pick words here, so the global tap-is-Confirm injection must stay off
  // (it would anchor or finish the selection at the current cursor instead).
  bool handlesDirectTouch() const override { return true; }
  // Keep the bar's Confirm: taps move the word selection, and Confirm is what
  // closes the range and saves the clip.
  bool tapActivatesConfirm() const override { return false; }
  // Range selection follows a dragging finger; the drag's closing swipe must
  // not be injected as Back/Confirm. Lookup mode keeps the Back swipe, and only
  // claims the contact once it has become a phrase drag (see handleLookupDrag).
  bool ownsSwipes() const override {
#if FREEINK_CAP_TOUCH
    return !lookupMode || dragging || contactAdopted || selectMode;
#else
    return !lookupMode;
#endif
  }

 private:
  WordList wordList;
  int fontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;
  bool lookupMode = false;
  bool selectMode = false;
  bool lookupAvailable = false;
  // Lookup mode: the current Confirm press began on this screen (the press that
  // opened it may still be down), and it has already been spent on anchoring.
  bool confirmPressedHere = false;
  bool anchorHoldTriggered = false;

  int currentDisplayPage = -1;
  int savedSectionPage = 0;
  // Cache the deserialized page so same-page cursor moves skip the SD read + deserialize and
  // only re-blit the glyphs. Reloaded only when the cursor crosses to a different page.
  std::unique_ptr<Page> cachedPage;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  mutable std::array<std::string, 4> prewarmTextByStyle;

  ButtonNavigator buttonNavigator;

#if FREEINK_CAP_TOUCH
  // Touch range selection state (handleRangeTouch).
  bool touchWasHeld = false;
  bool dragging = false;
  int contactStartWord = -1;
  // The start was set by a tap or drag here, not inherited from the reader's
  // hold -- decides whether tapping the start word saves it or confirms it.
  bool startTapped = false;
  // The contact being tracked began in the reader (see the constructor). Set
  // until that contact ends; while it is set nothing but the drag tracker may
  // act on input.
  bool contactAdopted = false;
  // Runs the drag tracker for an adopted contact. True while it owns the frame.
  bool handleAdoptedContact();

  // Drag and tap-tap range selection. True when it consumed this frame.
  bool handleRangeTouch();
  // Lookup mode: a drag that starts on the cursor word selects a phrase and
  // looks it up on lift. True while it owns the contact.
  bool handleLookupDrag();

  // --- Select mode (handles + toolbar) ---
  struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(const int px, const int py) const { return w > 0 && px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct SelectHandle {
    Box stem, dot, hit;
  };
  static constexpr int ACTION_LOOKUP = 0;
  static constexpr int ACTION_CLIP = 1;
  static constexpr int ACTION_COUNT = 2;
  // Geometry of the overlay for the current range. Pure function of the word
  // boxes and the UI font, so render() paints and loop() hit-tests the same
  // rectangles without caching anything between the two tasks.
  struct SelectLayout {
    SelectHandle start, end;
    Box bar;
    Box actions[ACTION_COUNT];  // an empty box = action hidden
    int pointerX = 0;
    bool pointerUp = false;  // bar below the selection
    bool showPointer = false;
  };
  enum class SelectDrag : uint8_t { None, Start, End };
  SelectDrag selectDrag = SelectDrag::None;
  // Finger-to-word-centre offset when a handle drag began, so the end being
  // dragged does not jump to whatever word sits under the fingertip.
  int dragOffsetX = 0;
  int dragOffsetY = 0;
  // Toolbar segment the current contact came down on; a tap acts only when it
  // lifts on the same one.
  int pressedAction = -1;
  SelectLayout selectLayout() const;
  Box wordBox(int idx) const;
  bool handleSelectTouch();
  void drawSelectOverlay(const SelectLayout& layout, bool showActions) const;
  void runSelectAction(int action);
#endif

  // Anchor at the cursor if unset, then build and return the clipping for the
  // anchored range. Shared by the Confirm button and the touch finish paths.
  void finishSelection();
  // Lookup mode's finish: the cursor word, or the anchored range joined into a phrase.
  void finishLookup();

  bool renderSelectionPage(int pageIdx);
  void prewarmHighlightedWords() const;
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  // Up/Down move by line (word-level stepping is Left/Right): to the first word of the next /
  // previous line.
  int lineForward(int idx) const;
  int lineBackward(int idx) const;
};
