#pragma once

#include <Epub/FootnoteEntry.h>

#include <cstring>
#include <functional>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes)
      : Activity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // The empty-footnote screen has no rows to hit-test; leave taps as injected
  // Confirm there.
  bool handlesDirectTouch() const override { return !footnotes.empty(); }

 private:
  // Row pitch; row i of the scroll window spans
  // [listTopY() + i*ROW_H, + ROW_H). Shared by render() and the Full Touch tap
  // hit-testing in loop().
  static constexpr int ROW_H = 36;
  int listTopY() const;
  int visibleRows() const;

  // The Confirm action for the selected footnote, also fired by a Full Touch tap
  // on the already-selected row.
  void activateSelectedFootnote();

  const std::vector<FootnoteEntry>& footnotes;
  int selectedIndex = 0;
  int scrollOffset = 0;
  ButtonNavigator buttonNavigator;
};
