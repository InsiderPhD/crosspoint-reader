#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Settings > OPDS Browser: the saved catalogs (OpdsServerStore), one row each,
 * then "Add Server". Confirm opens OpdsServerEditActivity for that catalog, or
 * for a new one. Each edit is saved by the editor when it closes.
 */
class OpdsServerListActivity final : public Activity {
 public:
  OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OpdsServerList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  // Server rows, plus the Add row while the store has room.
  int rowCount() const;
  bool isAddRow(int row) const;
  void activateRow(int row);
  Rect listRect() const;

  int selectedRow = 0;
  ButtonNavigator buttonNavigator;
};
