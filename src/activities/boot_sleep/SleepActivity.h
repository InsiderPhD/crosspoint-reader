#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;

  // The selected reading stats are drawn by the shared SleepStatsCard component
  // (also used by the settings preview); see activities/boot_sleep/SleepStatsCard.h.

  bool fromTimeout = false;
};
