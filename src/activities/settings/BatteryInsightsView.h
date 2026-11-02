#pragma once

#include <cstdint>

class GfxRenderer;

// Body of Settings > System > Battery: the current level, awake-on-battery
// time and days for the current charge cycle, battery level plotted against
// that awake time, and reader page turns. Reads util/PowerHistory;
// allocation-free.
namespace BatteryInsightsView {

// Fills the given rectangle; the graph takes whatever height the other two
// cards leave. batteryPercent is the level right now (0-100).
void draw(GfxRenderer& renderer, int x, int y, int width, int height, uint8_t batteryPercent);

}  // namespace BatteryInsightsView
