#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Submenu for KOReader Sync settings.
 * Shows username, password, and authenticate options.
 */
class KOReaderSettingsActivity final : public Activity {
 public:
  explicit KOReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("KOReaderSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  void handleSelection();
  Rect listRect() const;
};
