#pragma once

#include <GfxRenderer.h>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct ReadingBookStats;

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int currentPage = 0;
  // Mirrors SettingsActivity's pattern: index 0 = "on the ribbon" (the tab
  // bar at the top), 1+ = highlighted content item on the current page.
  // Resets when switching tabs (1 if previous was on an item, 0 if was on the
  // ribbon, with clamp to the new page's item count).
  int selectedItemIndex = 0;
  // Tabs whose content can exceed a screenful (Overview: stat rows + Reading
  // Profile + annual chart; Weekly: large goal boxes + rows + chart) scroll
  // vertically. Up/Down/Left/Right step the offset; maxScroll is recomputed by
  // the current page's render and is 0 on non-scrolling tabs.
  int scrollOffset = 0;
  int maxScroll = 0;
  // Stats always render portrait for a consistent layout; if the caller's
  // orientation differs we force Portrait on enter and restore it on exit.
  GfxRenderer::Orientation entryOrientation = GfxRenderer::Orientation::Portrait;
  bool restoreOrientationOnExit = false;
  bool fullRefreshNext = false;
  int viewedYear = 0;
  unsigned viewedMonth = 1;
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;
  // Number of selectable rows on the current page (excluding the ribbon).
  // For Overview/Weekly/Monthly this is 0; for Books/Sessions it's the
  // (capped) visible row count.
  int currentPageItemCount() const;
  void openSelectedBook();
  void confirmRemoveSelectedBook();
  void openSelectedSessionEditor();
  void changePage(int delta);
  void changeViewedMonth(int delta);
  void guardBackReturn();

 public:
  explicit ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
