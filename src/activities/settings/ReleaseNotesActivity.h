#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// What's New / Release Notes: the ReleaseNotes items as a scrolling page.
// Opened once after an update (Home, first visit) and from Settings > System
// > Release Notes. Leaving it marks the current notes as seen.
//
// Heap: the wrapped lines (~60 short strings, ~3KB) are built once in
// onEnter for the current orientation and freed with the activity; the note
// text itself stays in flash.
class ReleaseNotesActivity final : public Activity {
 public:
  explicit ReleaseNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReleaseNotes", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Line {
    std::string text;
    bool heading = false;
  };
  ButtonNavigator buttonNavigator;
  std::vector<Line> lines;
  int firstLine = 0;
  int linesPerPage = 1;

  void close();
};
