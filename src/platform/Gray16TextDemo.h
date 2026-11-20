#pragma once

// Dev-only (Picco builds with ENABLE_SERIAL_LOG): a portrait test page for the
// panel's 16-level path -- the 16 levels as patches, then the same paragraph
// anti-aliased with 16, 4 and 2 levels -- so text quality can be compared on
// the glass. Rendered straight from a TrueType file's 8-bit coverage
// (stb_truetype), not through GfxRenderer, which only draws 1-bit planes.

#include <cstdint>

#if FREEINK_DEVICE_PICCO && defined(ENABLE_SERIAL_LOG)
namespace gray16demo {

// Fill `levels` (4 bpp, panelW x panelH panel-native rows, two pixels per byte,
// high nibble = left pixel; 0 = white .. 15 = darkest) with the test page, laid
// out for the device held in portrait. `levelLstar` is the measured L* of each
// level, used to map glyph coverage to the nearest level in lightness.
// fontPath: a .ttf/.otf on the card, or nullptr for the first one in /fonts.
// Returns false (and logs why) when no font could be loaded.
bool render(uint8_t* levels, uint16_t panelW, uint16_t panelH, const char* fontPath, const float levelLstar[16]);

}  // namespace gray16demo
#endif
