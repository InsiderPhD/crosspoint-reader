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
  // singleWordMode turns the two-stage anchor->finish into a one-press pick and
  // returns a WordPickResult instead of a ClippingResult — the reader's
  // dictionary lookup needs one word, not a range, and none of the surrounding
  // context ClipTextBuilder assembles. Everything else (cursor movement, touch,
  // rendering) is deliberately shared, so there is only ever one word selector
  // on the page to keep working.
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WordList wordList, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft,
                        int initialCursorIdx = 0, bool preAnchored = false, bool singleWordMode = false);

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

 private:
  WordList wordList;
  int fontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;
  bool singleWordMode = false;

  int currentDisplayPage = -1;
  int savedSectionPage = 0;
  // Cache the deserialized page so same-page cursor moves skip the SD read + deserialize and
  // only re-blit the glyphs. Reloaded only when the cursor crosses to a different page.
  std::unique_ptr<Page> cachedPage;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  mutable std::array<std::string, 4> prewarmTextByStyle;

  ButtonNavigator buttonNavigator;

  // Anchor at the cursor if unset, then build and return the clipping for the
  // anchored range. Shared by the Confirm button and the touch finish paths.
  void finishSelection();

  bool renderSelectionPage(int pageIdx);
  void prewarmHighlightedWords() const;
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  // Up/Down move by line (word-level stepping is Left/Right): to the first word of the next /
  // previous line.
  int lineForward(int idx) const;
  int lineBackward(int idx) const;
};
