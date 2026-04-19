#pragma once

#include <cstdint>

#include "../Activity.h"

class ReadingStatsActivity final : public Activity {
  // Static in-memory cache: survives across activity instances within the same boot session.
  // Eliminates all SD I/O on re-entry. Reset on explicit user refresh.
  static bool hasCachedData;
  static uint64_t sCachedTotalBytes;
  static int64_t sCachedFreeBytes;
  static int sCachedEpubCount;

  bool isLoading = false;
  TaskHandle_t computeTaskHandle = nullptr;
  // Set true by the task AFTER requestUpdate(), BEFORE vTaskDelete(nullptr).
  // Lets onExit() skip vTaskDelete on a handle that is already self-deleted.
  volatile bool taskCompleted = false;

  void startComputeTask();
  static void computeTaskFunc(void* param);

 public:
  // Invalidate the in-memory cache (e.g. after a stats reset) so next entry recomputes.
  static void invalidateCache() { hasCachedData = false; }
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
