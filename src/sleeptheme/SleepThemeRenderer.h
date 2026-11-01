#pragma once

class GfxRenderer;

// Paints a compiled sleep theme (see SleepThemeFormat.h) into the framebuffer:
// the 1-bit background, then every live element with values resolved now.
// Does not refresh the display.
//
// Everything it allocates (the layout, the theme fonts, per-element transform
// scratch) is released before it returns.
namespace SleepThemeRenderer {

inline constexpr char THEMES_DIR[] = "/sleep-themes";

// Sets the renderer orientation to the theme's. False when the theme is
// missing or unusable (logged); the framebuffer may then hold a partial draw
// and the caller should paint its fallback over it.
bool draw(GfxRenderer& renderer, const char* themeName);

}  // namespace SleepThemeRenderer
