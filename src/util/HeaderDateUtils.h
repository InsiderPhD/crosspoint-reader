#pragma once

#include <string>

class GfxRenderer;

namespace HeaderDateUtils {

struct DisplayDateInfo {
  uint32_t timestamp = 0;
  bool usedFallback = false;  // true when falling back to APP_STATE.lastKnownValidTimestamp
};

DisplayDateInfo getDisplayDateInfo();
// Returns the formatted date text. Prepends "?" when the source is a stale fallback
// (no NTP/manual-date sync this boot) so the user knows the clock might be wrong.
std::string getDisplayDateText();

// Draws the date in the header's top-line area on the LEFT (at contentSidePadding).
// Battery + battery-percentage are placed on the right by the theme's drawHeader.
// Safe to call from any theme's drawHeader — the reader uses drawStatusBar instead
// so its UI is unaffected.
void drawTopLine(GfxRenderer& renderer);

// Convenience wrapper: draws GUI.drawHeader + the date top-line in one call.
void drawHeaderWithDate(GfxRenderer& renderer, const char* title, const char* subtitle = nullptr);

}  // namespace HeaderDateUtils
