#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * The custom combo slots, one row each: the chord and the action it runs.
 *
 * Confirm on a row opens ReaderComboWizardActivity for that slot; the wizard
 * hands back the chord and action and this screen writes them, so all four
 * slots cost a single settings save on exit (SPIFFS write throttling).
 *
 * Clearing a slot is "bind it to None" — the wizard's action picker offers it,
 * and sanitizeReaderCombos() collapses that to an empty slot on the next load.
 */
class ReaderComboListActivity final : public Activity {
 public:
  ReaderComboListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReaderComboList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  Rect listRect() const;
  void editSlot(uint8_t slot);

  int selectedRow = 0;
  bool isDirty = false;
  ButtonNavigator buttonNavigator;
};
