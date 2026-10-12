#pragma once

#include <Epub/PageList.h>

#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// "Go to Page": pick one of the publisher's page numbers (the print edition's,
// from the book's page list -- see PageList.h) and jump to it. The sibling of
// EpubReaderPercentSelectionActivity and drawn the same way: a popup card over
// whatever screen launched it, keeping no copy of that screen.
//
// Page labels are not always numbers ("xiv", "A-3"), so the selection is an
// index into the list and the label shown is read from the sidecar, one
// 64-byte record per step. Nothing of the list is held in memory.
//
// Returns PageResult{page = index into the page list}.
class EpubReaderPageSelectionActivity final : public Activity {
 public:
  explicit EpubReaderPageSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string cachePath, const uint16_t pageCount, const uint16_t initialIndex)
      : Activity("EpubReaderPageSelection", renderer, mappedInput),
        cachePath(std::move(cachePath)),
        pageCount(pageCount),
        index(initialIndex < pageCount ? initialIndex : 0) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  const std::string cachePath;
  const uint16_t pageCount;
  uint16_t index;
  // Label of `index`, refreshed by loadLabel() whenever the selection moves.
  char label[PageList::LABEL_SIZE] = "";

  ButtonNavigator buttonNavigator;

  void loadLabel();
  // Move the selection by delta entries, clamped to the list.
  void adjust(int delta);
};
