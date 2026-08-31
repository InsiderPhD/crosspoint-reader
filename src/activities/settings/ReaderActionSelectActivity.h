#pragma once

#if FREEINK_DEVICE_X4PRO

#include <GfxRenderer.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;
struct Rect;

/**
 * Picker for one reader action, opened from a Reader Controls row.
 *
 * X4 Pro only, and deliberately so: on the X3/X4 a row cycles its action on
 * Confirm, which is one press per step but wears no button out faster than any
 * other menu. Turning that into open-menu/scroll/select would triple the
 * presses on hardware whose buttons are the thing we are trying to spare. The
 * X4 Pro has no front buttons — the same interaction is taps — so it gets the
 * list instead of a cycle that has to be tapped through blind.
 *
 * It owns no persistence: the caller passes the row's current action and gets
 * the chosen one back as a ReaderActionResult, so Reader Controls keeps its
 * single batched save on exit. Backing out returns a cancelled result.
 */
class ReaderActionSelectActivity final : public Activity {
 public:
  ReaderActionSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* rowTitle,
                             uint8_t currentAction);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  Rect listRect() const;
  void confirmSelection();
  void cancel();

  // Copy, not a pointer: getRowTitle() hands back a shared static buffer.
  char rowTitle[48] = {};
  const uint8_t currentAction;
  // Offerable actions in enum order, minus the retired values and (outside Dev
  // Mode) Screenshot. Fixed capacity, so no heap and no vector growth.
  uint8_t actions[CrossPointSettings::READER_ACTION_COUNT] = {};
  uint8_t actionCount = 0;
  // Index of currentAction within actions[], or -1 when the stored slot holds a
  // value the picker does not offer.
  int currentIndex = -1;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
};

#endif  // FREEINK_DEVICE_X4PRO
