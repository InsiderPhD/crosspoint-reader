#pragma once

#include <HalFrontlight.h>

#if FREEINK_CAP_FRONTLIGHT

#include <GfxRenderer.h>

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;
struct Rect;

/**
 * Frontlight brightness picker: a list of the FrontlightLevels rungs, previewed
 * live on the panel as the cursor moves.
 *
 * It owns no persistence. The caller passes the level to start from and gets the
 * chosen one back as a FrontlightResult, so the reader menu can keep batching its
 * writes to menu exit and Settings can write once on confirm. Backing out restores
 * the light to the level the picker opened on and returns a cancelled result.
 */
class FrontlightBrightnessActivity final : public Activity {
 public:
  FrontlightBrightnessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t brightness,
                               uint8_t warmth)
      : Activity("FrontlightBrightness", renderer, mappedInput), startBrightness(brightness), warmth(warmth) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  void moveTo(int index);
  void confirmSelection();
  void cancel();
  Rect listRect() const;

  const uint8_t startBrightness;
  const uint8_t warmth;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
};

#endif  // FREEINK_CAP_FRONTLIGHT
