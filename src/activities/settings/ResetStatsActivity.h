#pragma once

#include "activities/Activity.h"

class ResetStatsActivity final : public Activity {
 public:
  explicit ResetStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ResetStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, RESETTING, SUCCESS, FAILED };

  State state = WARNING;

  void resetStats();
};
