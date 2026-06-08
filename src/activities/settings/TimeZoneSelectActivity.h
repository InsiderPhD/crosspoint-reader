#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TimeZoneSelectActivity final : public Activity {
 public:
  explicit TimeZoneSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TimeZoneSelect", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
};
