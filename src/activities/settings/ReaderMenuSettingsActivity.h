#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Show/hide picker for every row of the reader's in-book menu.
//
// One row per entry the menu can build (ReaderMenuVisibility::kRows), Confirm
// toggles it, plus a "Show All" row at the bottom to undo an over-enthusiastic
// trim. Rows this device could never show -- the frontlight pair on a board
// without a light, the Dev Mode aids -- are left out entirely.
class ReaderMenuSettingsActivity final : public Activity {
 public:
  explicit ReaderMenuSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderMenuSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  ButtonNavigator buttonNavigator;

  // Indices into ReaderMenuVisibility::kRows that are available on this device,
  // resolved in onEnter(). Dev Mode and the frontlight can't change while the
  // screen is open, so this never needs rebuilding.
  std::vector<uint8_t> visibleRows;
  int selectedIndex = 0;
  bool isDirty = false;

  // Row count including the trailing "Show All" row.
  int rowCount() const { return static_cast<int>(visibleRows.size()) + 1; }
  bool isShowAllRow(int index) const { return index == static_cast<int>(visibleRows.size()); }

  void handleSelection();
  Rect listRect() const;
};
