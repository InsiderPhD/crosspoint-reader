#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class ReaderControlsActivity final : public Activity {
 public:
  explicit ReaderControlsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderControls", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

  // Returns the localized action name for a READER_ACTION enum value. Public + static so
  // the reader can reuse it when drawing the button-hint bar.
  static const char* actionName(CrossPointSettings::READER_ACTION action);

 private:
  Rect listRect() const;

  // Rows 14-18 are the X4 Pro touch extras: the three tap zones, home key
  // short and long press. Rows 19-21 are the hold (long-press) variants of the
  // three tap zones, and row 22 picks the axis those zones are cut along.
  // Row 23 is Custom Combos on every board: not an action slot of its own, it
  // opens the chord list. Row 24 is the X4 Pro home key's double tap.
  static constexpr uint8_t kTotalRows = 25;

  uint8_t selectedRow = 0;
  bool isDirty = false;
  ButtonNavigator buttonNavigator;

  // Returns the label for each row (button + press type).
  const char* getRowTitle(uint8_t row) const;
  // Returns the action label for each row.
  const char* getRowActionName(uint8_t row) const;
  // Confirm/tap on a row: opens the action picker (the X4 Pro's tap-zone axis
  // row toggles in place instead).
  void activateRow(uint8_t row);
  // Custom combo slots in use, for the Custom Combos row's value.
  static uint8_t definedComboCount();
  // Opens the action picker for the given row and stores what comes back.
  void openActionPicker(uint8_t row);
  // Settings field backing a row, or nullptr for the rows whose action is fixed
  // (Power long press, Home hold). Single source for both reading a row's
  // current action and writing the picked one.
  uint8_t* fieldForRow(uint8_t row) const;
  // Returns the current action for a row.
  CrossPointSettings::READER_ACTION getActionForRow(uint8_t row) const;
};
