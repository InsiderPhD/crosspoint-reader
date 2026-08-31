#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class ManualDateActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedField = 0;
  int year = 2026;
  unsigned month = 6;
  unsigned day = 15;

  void adjustSelectedField(int delta);
  void saveDate();
  std::string getSelectedDateLabel() const;
  Rect listRect() const;

 public:
  explicit ManualDateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ManualDate", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }
  // Keep the bar's Confirm: a tap on the highlighted field steps its value
  // (the Right button's job), so committing the date has no other affordance.
  bool tapActivatesConfirm() const override { return false; }
};
