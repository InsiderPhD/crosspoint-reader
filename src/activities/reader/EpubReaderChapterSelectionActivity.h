#pragma once
#include <Epub.h>

#include <memory>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int selectorIndex = 0;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;
  // Band the rows are laid out in; its bottom strip holds the page count.
  Rect listAreaRect() const;

  // Total TOC items count
  int getTotalItems() const;

  // Row pitch; row i spans [listTopY() + i*ROW_H, + ROW_H). Shared by render()
  // and the Full Touch tap hit-testing in loop().
  static constexpr int ROW_H = 30;
  int listTopY() const;

  // The Confirm action for the selected chapter, also fired by a Full Touch tap
  // on the already-selected row.
  void activateSelectedChapter();

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const int currentSpineIndex)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handlesDirectTouch() const override { return true; }
};
