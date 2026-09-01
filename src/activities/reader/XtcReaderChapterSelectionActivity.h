#pragma once
#include <Xtc.h>

#include <memory>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  ButtonNavigator buttonNavigator;
  uint32_t currentPage = 0;
  int selectorIndex = 0;

  int getPageItems() const;
  // Band the rows are laid out in; its bottom strip holds the page count.
  Rect listAreaRect() const;
  int findChapterIndexForPage(uint32_t page) const;

  // Row pitch; row i spans [listTopY() + i*ROW_H, + ROW_H). Shared by render()
  // and the Full Touch tap hit-testing in loop().
  static constexpr int ROW_H = 30;
  int listTopY() const;

  // The Confirm action for the selected chapter, also fired by a Full Touch tap
  // on the already-selected row.
  void activateSelectedChapter();

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput), xtc(xtc), currentPage(currentPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // The empty-chapter screen has no rows to hit-test; leave taps as injected
  // Confirm there.
  bool handlesDirectTouch() const override { return xtc && !xtc->getChapters().empty(); }
};
