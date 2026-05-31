#pragma once

#include "../Activity.h"
#include "util/ButtonNavigator.h"

struct ReadingBookStats;

class ReadingStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int currentPage = 0;
  int selectedBookIndex = 0;
  int viewedYear = 0;
  unsigned viewedMonth = 1;
  bool waitForConfirmRelease = false;
  bool waitForBackRelease = false;
  void openSelectedBook();
  void confirmRemoveSelectedBook();
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
