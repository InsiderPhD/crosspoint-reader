#pragma once

#include "activities/Activity.h"

// Settings > System > Battery: the current level, then battery level against
// awake-on-battery time since the last charge (util/PowerHistory). Read-only;
// holds no state beyond the level read when the screen opens.
class BatteryActivity final : public Activity {
 public:
  explicit BatteryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Battery", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  uint8_t batteryPercent = 0;
};
