#pragma once

#include <cstddef>
#include <cstdint>
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

// The Outside Reader bar's two time elements, formatted into caller buffers
// (BaseTheme::drawHeaderStatusBar lays them out). formatDate writes the same
// text getDisplayDateText() returns. formatClock writes nothing when the
// timestamp is a stale fallback: a stale date is still right most of the day,
// a stale time never is. Both return false (and an empty string) when there is
// nothing to show.
bool formatDate(const DisplayDateInfo& info, char* out, size_t outLen);
bool formatClock(const DisplayDateInfo& info, char* out, size_t outLen);

// Thin wrapper over GUI.drawHeader, which draws the Outside Reader bar itself.
void drawHeaderWithDate(GfxRenderer& renderer, const char* title, const char* subtitle = nullptr);

}  // namespace HeaderDateUtils
