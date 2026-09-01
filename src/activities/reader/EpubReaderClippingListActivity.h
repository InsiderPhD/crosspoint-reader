#pragma once

#include <string>
#include <vector>

#include "ClippingStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Lists the current book's saved clippings. Up/Down scroll, Confirm opens a paged detail view
// (Confirm again jumps to the clipping's location), Back exits, and holding Confirm asks to
// delete the selected clipping. Returns a ClippingJumpResult when the user jumps.
class EpubReaderClippingListActivity final : public Activity {
 public:
  // Holds a reference into the ClippingStore singleton rather than a copy: 64 clippings of up
  // to 512B text each is ~40KB of avoidable heap. The store stays loaded for the whole book
  // (the reader is this activity's parent), so the reference outlives the activity; deletions
  // through removeClippingAt() are visible immediately via the same vector.
  EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::vector<Clipping>& clippings)
      : Activity("EpubClippingList", renderer, mappedInput), clippings(clippings) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only the list has hit-testable rows: the paged detail view and the empty
  // state keep the injected-Confirm tap behaviour.
  bool handlesDirectTouch() const override { return !detailMode && !clippings.empty(); }

 private:
  const std::vector<Clipping>& clippings;
  std::string detailText;
  std::vector<std::string> detailLines;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int detailPage = 0;
  int detailLayoutWidth = 0;
  int detailLinesPerPage = 0;
  bool deleteHoldTriggered = false;
  bool detailMode = false;

  int getPageItems() const;
  // Band the rows are laid out in; its bottom strip holds the page count.
  Rect listAreaRect() const;
  // Top edge of the first list row band; row i spans
  // [listTopY() + i*ROW_HEIGHT, + ROW_HEIGHT). Shared by render() and the Full
  // Touch tap hit-testing in loop().
  int listTopY() const;
  int getDetailTextWidth() const;
  int getDetailLinesPerPage() const;
  int getDetailPageCount() const;
  // The hold-Confirm delete, also fired by a Full Touch long tap on a row.
  // Returns true when a clipping was actually removed.
  bool deleteSelectedClipping();
  void closeDetail();
  void jumpToSelectedClipping();
  void openSelectedDetail();
  void rebuildDetailLayoutIfNeeded();
  void renderDetail();
};
