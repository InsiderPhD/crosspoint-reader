#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Sleep-screen reading-stats configuration activity. Lists the three sleep-stat
// picker slots and renders a live preview of the resulting sleep-screen stat card
// (via SleepStatsCard) so the user can see what each selection looks like before
// leaving the device. Mirrors StatusBarSettingsActivity's list + live-preview layout.
class SleepStatsSettingsActivity final : public Activity {
 public:
  explicit SleepStatsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SleepStatsSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  bool changed = false;  // gate the single saveToFile() on exit

  void handleSelection();
};
