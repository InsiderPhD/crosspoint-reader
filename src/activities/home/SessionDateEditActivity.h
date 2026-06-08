#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Day/month/year picker for assigning a date to a sessionLog entry. Mirrors
// the ManualDateActivity UX but instead of setting the system clock it calls
// READING_STATS.editSessionDate(sessionIndex, newDayOrdinal). Used from the
// Sessions tab when the user wants to date an unsynced session or correct
// one whose dayOrdinal was based on a stale clock.
class SessionDateEditActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  size_t sessionIndex;
  int selectedField = 0;
  int year = 2026;
  unsigned month = 6;
  unsigned day = 15;

  void adjustSelectedField(int delta);
  void saveDate();
  std::string getSelectedDateLabel() const;

 public:
  SessionDateEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, size_t sessionIndex)
      : Activity("SessionDateEdit", renderer, mappedInput), sessionIndex(sessionIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
