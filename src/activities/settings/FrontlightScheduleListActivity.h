#pragma once

#include <HalFrontlight.h>

#if FREEINK_CAP_FRONTLIGHT

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * The frontlight schedule slots, one row each: the window and the level it
 * sets, or Off.
 *
 * Confirm on a row opens FrontlightScheduleEditActivity for that slot. The
 * editor writes the slot into SETTINGS and this screen owns the single
 * settings save on exit (SPIFFS write throttling), re-running the scheduler
 * straight away so a window that already contains "now" lights up without
 * waiting for the next tick.
 */
class FrontlightScheduleListActivity final : public Activity {
 public:
  FrontlightScheduleListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FrontlightScheduleList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  Rect listRect() const;
  void editSlot(uint8_t slot);
  void applySlotIfActive(uint8_t slot);

  int selectedRow = 0;
  bool isDirty = false;
  ButtonNavigator buttonNavigator;
};

#endif  // FREEINK_CAP_FRONTLIGHT
