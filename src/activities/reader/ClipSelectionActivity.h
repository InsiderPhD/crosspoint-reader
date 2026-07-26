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
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, WordList wordList, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  WordList wordList;
  int fontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;

  int currentDisplayPage = -1;
  int savedSectionPage = 0;
  // Cache the deserialized page so same-page cursor moves skip the SD read + deserialize and
  // only re-blit the glyphs. Reloaded only when the cursor crosses to a different page.
  std::unique_ptr<Page> cachedPage;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  mutable std::array<std::string, 4> prewarmTextByStyle;

  ButtonNavigator buttonNavigator;

  bool renderSelectionPage(int pageIdx);
  void prewarmHighlightedWords() const;
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  // Up/Down move by line (word-level stepping is Left/Right): to the first word of the next /
  // previous line.
  int lineForward(int idx) const;
  int lineBackward(int idx) const;
};
