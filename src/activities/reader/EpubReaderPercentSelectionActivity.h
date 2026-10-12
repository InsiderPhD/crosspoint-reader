#pragma once

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderPercentSelectionActivity final : public Activity {
 public:
  // Slider-style percent selector for jumping within a book. Drawn as a popup
  // card over whatever screen launched it (see render()); it keeps no copy of
  // that screen, so it must only be started from a screen that has already
  // pushed a frame and is not redrawn while this is open.
  explicit EpubReaderPercentSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const int initialPercent)
      : Activity("EpubReaderPercentSelection", renderer, mappedInput), percent(initialPercent) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  // Current percent value (0-100) shown on the slider.
  int percent = 0;

  ButtonNavigator buttonNavigator;

  // Change the current percent by a delta and clamp within bounds.
  void adjustPercent(int delta);
};
