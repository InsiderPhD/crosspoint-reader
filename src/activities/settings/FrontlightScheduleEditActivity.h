#pragma once

#include <HalFrontlight.h>

#if FREEINK_CAP_FRONTLIGHT

#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Editor for one frontlight schedule slot: Enabled, Start, End, Brightness and
 * (on warm/cool boards) Warmth. Enabled toggles in place; every other row
 * opens a picker list — the hours as a 24-row list, brightness through the
 * same FrontlightBrightnessActivity the reader menu uses, warmth as a list of
 * ten-point steps — so a value is chosen by scrolling to it, not tapped up to.
 * Brightness and warmth preview live on the panel inside their pickers.
 *
 * While the editor is open the panel is lit at the draft's level, so the
 * schedule can be judged as a whole, not one field at a time; the pickers
 * carry that preview on, and Back puts the current setting back.
 *
 * Works on a copy: Back writes it into SETTINGS.frontlightSchedules[slot] and
 * returns a plain result; persisting is the caller's job.
 */
class FrontlightScheduleEditActivity final : public Activity {
 public:
  FrontlightScheduleEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t slot)
      : Activity("FrontlightScheduleEdit", renderer, mappedInput), slot(slot) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  int fieldCount() const;
  void activateField(int index);
  void pickHour(bool start);
  void pickBrightness();
  void pickWarmth();
  void previewDraft();
  void restoreLight();
  void saveAndExit();
  Rect listRect() const;
  std::string fieldLabel(int index) const;
  std::string fieldValue(int index) const;

  const uint8_t slot;
  CrossPointSettings::FrontlightSchedule draft;
  int selectedField = 0;
  ButtonNavigator buttonNavigator;
};

#endif  // FREEINK_CAP_FRONTLIGHT
