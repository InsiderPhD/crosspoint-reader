#pragma once
#include "../Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;

  static constexpr size_t STAT_LINE_LEN = 64;
  static constexpr uint8_t MAX_STAT_LINES = 4;

  // One rendered stat line: either a text string, or (when weekDots is set) a row
  // of weekCount weekday cells for the weekly-streak stat (Mon..Fri of the current
  // week). weekMask bit d (0 = Monday) is set when that weekday met the daily goal;
  // each cell is labelled with its weekday letter, which the fill hides when met.
  struct StatLine {
    char text[STAT_LINE_LEN];
    bool weekDots;
    uint8_t weekMask;
    uint8_t weekCount;
  };

  // Formats the user-selected sleep-screen stats (slots 1-3) into `lines`,
  // skipping any slot set to SLEEP_STAT_NONE (and the "if found" slot when no
  // contact is set). Most stats are one line; the "if found" stat emits two (a
  // label line and the contact on its own line). Returns the number of lines
  // filled (0..MAX_STAT_LINES).
  uint8_t buildStatLines(StatLine lines[MAX_STAT_LINES]) const;
  // Draws the selected stats in a single bordered box centred in the
  // lower-middle of the screen. The white fill keeps the text legible over a
  // cover/wallpaper; on the dark default screen the caller's invertScreen()
  // flips it to a white outline + white text on black. No-op when no stats are
  // selected. Does not trigger a display refresh — the caller commits.
  //
  // clearRegionOnly is used by the greyscale cover path: the box is painted into
  // the BW base image once, then each greyscale plane pass clears the box
  // rectangle so displayGrayBuffer() leaves that region as the BW box while the
  // rest of the cover renders in greyscale (single refresh, greyscale preserved).
  void drawStatLines(bool clearRegionOnly = false) const;

  bool fromTimeout = false;
};
