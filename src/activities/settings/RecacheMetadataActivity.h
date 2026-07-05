#pragma once

#include "activities/Activity.h"

// Dev-mode action: walk the whole library and build book.bin metadata for any
// book that doesn't have it yet, so tags (and other metadata) exist for books
// that have never been opened. This is what makes Tag-folder mode complete —
// otherwise unopened books stay in "Untagged". Modelled on ClearCacheActivity:
// a WARNING confirm gate, a blocking RUNNING pass with progress, then a result.
class RecacheMetadataActivity final : public Activity {
 public:
  explicit RecacheMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecacheMetadata", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode during the pass.
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, RUNNING, SUCCESS };

  State state = WARNING;
  void goBack() { finish(); }

  // Progress + result counters (shown live during RUNNING, summarised in SUCCESS).
  int totalBooks = 0;
  int processed = 0;
  int builtCount = 0;
  int failedCount = 0;

  void recacheMetadata();
};
