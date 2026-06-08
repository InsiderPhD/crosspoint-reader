#pragma once

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
