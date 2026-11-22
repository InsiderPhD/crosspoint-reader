#pragma once

#if FREEINK_DEVICE_PICCO

#include <string>

class GfxRenderer;
namespace SleepThemeRenderer {
struct GreyPlan;
}

// Picco SE0400NQW47: a greyscale sleep theme's images at the panel's 16 levels
// instead of the 4-shade plane passes. The theme's B/W frame (already in the
// framebuffer) supplies black and white; each grey image is placed exactly as
// GfxRenderer::drawBitmap places it and dithered to the measured levels. The
// open book's cover comes from its 8-bit cached twin when it has one, so the
// full range of tones reaches the panel. One clean 16-level draw, ~2.4 s.
namespace SleepGray16 {

// Shows the sleep screen through the 16-level path. On return the framebuffer
// holds the B/W frame again (the USB charge-strip redraw starts from it).
// False, with nothing shown, when the path is unavailable (other panels, a
// theme with art in front of its images, no PSRAM): the caller then runs the
// 4-shade passes.
bool present(GfxRenderer& renderer, const SleepThemeRenderer::GreyPlan& plan);

// True once present() has put the 16-level screen up (until the next boot):
// anything that would refresh over it in B/W -- the USB charge strip -- must
// leave it alone.
bool isUp();

// One picture (any BMP; 8-bit gray keeps every tone) at the panel's 16 levels,
// fitted inside the screen and centred, over whatever the framebuffer holds
// (button hints), which is left untouched. False, with nothing shown, when the
// panel or the driver's measured tones are unavailable. The next frame gives
// the drive back.
bool showImage(GfxRenderer& renderer, const std::string& bmpPath);

// Deep sleep (and a restart) loses the display driver's copy of the 16-level
// screen, so the drive it put into the ink would never be given back and would
// build up sleep after sleep. saveOwedDrive() writes that state to the SD card
// just before sleeping (no-op when no 16-level screen is up);
// restoreOwedDrive() hands it back to the driver on the next boot, after
// display.begin() and before the first frame, which then gives the drive back.
// Where saveOwedDrive() keeps it; present only between that and the next boot.
inline constexpr char OWED_DRIVE_FILE[] = "/.crosspoint/gray16_owed.bin";
void saveOwedDrive();
void restoreOwedDrive();

}  // namespace SleepGray16

#endif
