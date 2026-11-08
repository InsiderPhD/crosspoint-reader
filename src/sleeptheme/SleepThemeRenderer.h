#pragma once

#include <cstdint>
#include <cstring>
#include <string>

class GfxRenderer;

// Paints a compiled sleep theme (see SleepThemeFormat.h) into the framebuffer:
// the 1-bit background, then every live element with values resolved now.
// Does not refresh the display.
//
// Everything it allocates (the layout, the theme fonts, per-element transform
// scratch) is released before it returns.
namespace SleepThemeRenderer {

inline constexpr char THEMES_DIR[] = "/sleep-themes";

// Themes that work without a download. When their folder is missing, "light"
// and "dark" draw the built-in logo screen they are copies of, "stats" and
// "stats-dark" add the three-line reading stats card under the logo (the look
// of the retired sleep-screen stats setting), and "cover-crop" is laid out by
// the firmware: the open book's cover zoomed to fill the screen, over the
// light logo screen when there is none.
inline constexpr const char* BUILT_IN_THEMES[] = {"light", "dark", "stats", "stats-dark", "cover-crop"};
inline bool isBuiltIn(const char* themeName) {
  for (const char* name : BUILT_IN_THEMES) {
    if (strcmp(themeName, name) == 0) return true;
  }
  return false;
}

// What the caller must know before drawing: an overlay keeps the screen that is
// showing (no popup, no scrub); greyscale images need the panel's grey passes.
struct ThemeInfo {
  bool overlay = false;
  bool greyscale = false;
  bool showsSleepImage = false;  // has an image element showing sleep.image
  uint16_t width = 0;            // the screen size it was laid out at
  uint16_t height = 0;
};
// Reads the theme's header and elements only. False when it is missing or not
// a theme this firmware reads.
bool inspect(const char* themeName, ThemeInfo& out);

// Images drawn in grey (data-filter="grey"). draw() paints them into the BW
// base; the caller then repeats them in each grey pass with drawGreyImages().
struct GreyImage {
  std::string path;
  int16_t x = 0, y = 0, width = 0, height = 0;  // where the image lands
  float cropX = 0, cropY = 0;                   // GfxRenderer::drawBitmap crop
  bool dither = false;                          // Bitmap dithering (any-depth sources)
};
struct GreyPlan {
  static constexpr uint8_t MAX_IMAGES = 4;
  GreyImage images[MAX_IMAGES];
  uint8_t count = 0;
  // Theme folder when its front.bmp art sits over images: the grey passes
  // leave those pixels as the BW base drew them. Empty otherwise.
  std::string frontDir;
};

// Sets the renderer orientation to the theme's. False when the theme is
// missing or unusable (logged); the framebuffer may then hold a partial draw
// and the caller should paint its fallback over it. With grey null, grey
// images are dithered instead.
bool draw(GfxRenderer& renderer, const char* themeName, GreyPlan* grey = nullptr);

// The logo screen "light" and "dark" are copies of (dark: inverted), drawn
// over the whole framebuffer in the renderer's current orientation, for when
// their folder isn't on the card.
void drawBuiltIn(GfxRenderer& renderer, bool dark);

// The screen the device draws when themeName can't be drawn: the logo screen,
// with the stats card for "stats"/"stats-dark", inverted for the dark ones.
void drawFallback(GfxRenderer& renderer, const char* themeName);

// A store install saves the store's screenshot of the theme in its folder
// under this name: a 2-bit BMP fitted within the panel's long side squared,
// so it serves either UI orientation. Shown by the theme pickers only; the
// sleep screen never reads it.
inline constexpr char PREVIEW_FILE[] = "preview.bmp";

// Draws a preview BMP fitted (aspect kept, centred, never enlarged) in the box,
// dithered, with a 1 px outline. negative draws it inverted, for a screen that
// is inverted as a whole afterwards (dark mode). False, drawing nothing, when
// the file is missing or unreadable.
bool drawPreview(GfxRenderer& renderer, const char* bmpPath, int boxX, int boxY, int boxW, int boxH, bool negative);

// Draws the plan's images in the renderer's current (grey) render mode.
void drawGreyImages(const GfxRenderer& renderer, const GreyPlan& plan);

// Called when the user picks a sleep image or folder: makes sure the sleep
// screen shows it, switching to the store's "custom" theme unless the current
// theme already shows sleep.image. Saves the settings when it changes them.
void showSleepImages();

}  // namespace SleepThemeRenderer
