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

#if FREEINK_DEVICE_X4PRO
  // Rows 14-18 are the X4 Pro touch extras: tap left/middle/right, home key
  // short and long press. Rows 19-21 are the hold (long-press) variants of the
  // three tap zones.
  static constexpr uint8_t kTotalRows = 22;
#else
  static constexpr uint8_t kConfigurableRows = 13;  // 13 user-configurable + 1 fixed
  static constexpr uint8_t kTotalRows = 14;
#endif

  uint8_t selectedRow = 0;
  bool isDirty = false;
  ButtonNavigator buttonNavigator;

  // Returns the label for each row (button + press type).
  const char* getRowTitle(uint8_t row) const;
  // Returns the action label for each row.
  const char* getRowActionName(uint8_t row) const;
  // Confirm/tap on a row. X4 Pro opens the action picker; everywhere else the
  // row cycles in place — see openActionPicker()'s note on button wear.
  void activateRow(uint8_t row);
#if FREEINK_DEVICE_X4PRO
  // Opens the action picker for the given row and stores what comes back.
  void openActionPicker(uint8_t row);
#else
  // Advances the action for the given row by one (wraps around).
  void cycleActionForRow(uint8_t row);
#endif
  // Settings field backing a row, or nullptr for the rows whose action is fixed
  // (Power long press, Home hold). Single source for both reading a row's
  // current action and writing the picked one.
  uint8_t* fieldForRow(uint8_t row) const;
  // Returns the current action for a row.
  CrossPointSettings::READER_ACTION getActionForRow(uint8_t row) const;
};
