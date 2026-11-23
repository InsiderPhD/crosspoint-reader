#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Show/hide picker for every row of the reader's in-book menu.
//
// Info-line toggles first (what the menu shows under its title), then one row
// per entry the menu can build (ReaderMenuVisibility::kRows), Confirm
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

  // The info-line toggles at the top (SETTINGS.readerMenuInfo).
  static constexpr int INFO_ROWS = 7;

 private:
  ButtonNavigator buttonNavigator;

  // Indices into ReaderMenuVisibility::kRows that are available on this device,
  // resolved in onEnter(). Dev Mode and the frontlight can't change while the
  // screen is open, so this never needs rebuilding.
  std::vector<uint8_t> visibleRows;
  int selectedIndex = 0;
  bool isDirty = false;

  // The menu's info lines (SETTINGS.readerMenuInfo) come first, then the menu
  // rows, then "Show All".
  int rowCount() const { return INFO_ROWS + static_cast<int>(visibleRows.size()) + 1; }
  bool isInfoRow(int index) const { return index < INFO_ROWS; }
  bool isShowAllRow(int index) const { return index == rowCount() - 1; }
  // kRows index of a menu row (callers check isInfoRow / isShowAllRow first).
  uint8_t menuRowAt(int index) const { return visibleRows[index - INFO_ROWS]; }

  void handleSelection();
  Rect listRect() const;
};
