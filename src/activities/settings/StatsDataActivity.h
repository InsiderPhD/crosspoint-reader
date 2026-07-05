#pragma once

#include "activities/Activity.h"

// Confirmation + progress screen for the reading-stats data actions on the
// Stats settings tab. One class, three modes (mirrors ResetStatsActivity):
//   - ExportJson: lossless JSON export (round-trip / backup)
//   - ImportJson: restore from the JSON export
//   - ExportStoryGraph: one-way StoryGraph-compatible CSV of the book catalog
enum class StatsDataMode { ExportJson, ImportJson, ExportStoryGraph };

class StatsDataActivity final : public Activity {
 public:
  StatsDataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StatsDataMode mode)
      : Activity("StatsData", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, WORKING, SUCCESS, FAILED, NO_FILE };

  StatsDataMode mode;
  State state = WARNING;

  void runAction();
  const char* titleText() const;
  const char* confirmText() const;
  const char* workingText() const;
  const char* successText() const;
  const char* failText() const;
};
