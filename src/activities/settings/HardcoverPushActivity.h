#pragma once

#include <cstdint>
#include <vector>

#include "activities/Activity.h"

// Settings > Stats > Push to Hardcover. Pushes every stats book whose progress
// Hardcover doesn't have yet, behind one static frame with the framebuffer lent
// to TLS for the whole batch (like the download flows), so the panel rails stay
// off and nothing repaints until the summary.
//
// Flow: confirm (with the count) -> WiFi selection -> push -> summary. When
// nothing is pending, "Push again" re-sends every book from scratch (for when
// its data was deleted on Hardcover while testing).
class HardcoverPushActivity final : public Activity {
 public:
  HardcoverPushActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HardcoverPush", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == PUSHING; }
  bool skipLoopDelay() override { return state == PUSHING; }

 private:
  enum State { NO_TOKEN, NOTHING_TO_PUSH, CONFIRM, CONNECTING, PUSHING, DONE, FAILED };

  State state = CONFIRM;
  // Indices into READING_STATS.getBooks(). Settings never runs a reading
  // session, so the vector can't change under us while this screen is open.
  std::vector<uint16_t> pending;
  // Push again: every eligible book, with its saved sync state forgotten first.
  bool forceAll = false;
  unsigned updated = 0;
  unsigned notFound = 0;
  unsigned failed = 0;
  // Translated message for the error that ended the run early; read in FAILED.
  const char* failMessage = nullptr;

  void collectPending(bool includeUpToDate);
  void startPush();
  void pushAll();
};
