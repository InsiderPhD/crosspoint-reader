#pragma once
#include "../Activity.h"

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  // The built-in screen (logo, "Sleeping"): shown when no theme is set or the
  // chosen one is missing or unusable.
  void renderDefaultSleepScreen() const;
  void renderLastScreenSleepScreen() const;
  // Compiled HTML/CSS theme (SETTINGS.sleepTheme). greyscale: it has images in
  // the panel's greys, which need the grey passes after the BW paint.
  void renderThemeSleepScreen(bool greyscale) const;
  // Overlay theme: keeps the framebuffer as the outgoing screen left it and
  // paints the theme over it.
  void renderOverlayThemeSleepScreen() const;

  bool fromTimeout = false;
};
