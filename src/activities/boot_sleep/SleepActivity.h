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
  // Picks an image from dirPath (rotation history in APP_STATE) and paints it.
  // False when the folder is missing or has no usable image; nothing is drawn.
  bool renderSleepImageFromFolder(const char* dirPath) const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  // Page Overlay: keeps the framebuffer as the outgoing screen left it and
  // composites a transparent image from the SD card over it.
  void renderOverlaySleepScreen() const;
  // Draws the overlay image, if there is one. False when nothing was drawn.
  bool drawSleepOverlay() const;

  // The selected reading stats are drawn by the shared SleepStatsCard component
  // (also used by the settings preview); see activities/boot_sleep/SleepStatsCard.h.

  bool fromTimeout = false;
};
