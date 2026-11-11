#pragma once

#include <vector>

#include "ClippingStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Every book with saved clippings, Kindle style: one row per book (title -
// author, clipping count). Confirm opens that book's clippings
// (EpubReaderClippingListActivity, standalone), where each clipping opens its
// full text and can jump into the book. Reached from Home (Licorice's
// clipping card).
//
// Only the book list is held here (ClippedBookEntry, read from each clipping
// file's header); clippings themselves are loaded one book at a time by the
// per-book list, so the cost is the same as Stats > Clippings.
class ClippingsActivity final : public Activity {
 public:
  ClippingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Clippings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return !books.empty(); }

 private:
  std::vector<ClippedBookEntry> books;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  // Swallow the Back release that closed the per-book list, so it does not
  // also close this screen.
  bool waitForBackRelease = false;

  void loadBooks();
  void openSelectedBook();
  Rect listRect() const;
};
