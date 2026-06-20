#pragma once

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReaderControlsActivity final : public Activity {
 public:
  explicit ReaderControlsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderControls", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Returns the localized action name for a READER_ACTION enum value. Public + static so
  // the reader can reuse it when drawing the button-hint bar.
  static const char* actionName(CrossPointSettings::READER_ACTION action);

 private:
  static constexpr uint8_t kConfigurableRows = 13;  // 13 user-configurable + 1 fixed
  static constexpr uint8_t kTotalRows = 14;

  uint8_t selectedRow = 0;
  bool isDirty = false;
  ButtonNavigator buttonNavigator;

  // Returns the label for each row (button + press type).
  const char* getRowTitle(uint8_t row) const;
  // Returns the action label for each row.
  const char* getRowActionName(uint8_t row) const;
  // Advances the action for the given row by one (wraps around).
  void cycleActionForRow(uint8_t row);
  // Returns the current action for a row.
  CrossPointSettings::READER_ACTION getActionForRow(uint8_t row) const;
};
