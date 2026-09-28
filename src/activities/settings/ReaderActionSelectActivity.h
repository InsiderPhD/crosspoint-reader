#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;
struct Rect;

/**
 * Picker for one reader action.
 *
 * Reader Controls opens it for every action row on every board, and the Custom
 * Combo wizard uses it for a captured chord. The one-line description under
 * each action is why it beats cycling a row in place, even on boards whose
 * front buttons make the cycle one press per step.
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
