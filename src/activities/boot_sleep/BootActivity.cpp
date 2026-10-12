#include "BootActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointState.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {

// Draws the user's boot image into the framebuffer. False = nothing usable was
// drawn and the caller shows the built-in logo instead.
//
// This runs on every cold boot that shows a boot screen, so the common case
// has to cost nothing: with no image chosen it returns on the empty-path check
// without touching the card. A chosen image costs one open plus the BMP read,
// which is the time the user opted into (a 1-bit 480x800 BMP is 48KB, a 24-bit
// one 1.1MB). drawBitmap streams it a row at a time through a few hundred
// bytes of scratch that it frees before returning.
bool drawCustomBootImage(GfxRenderer& renderer) {
  const std::string& path = APP_STATE.bootImagePath;
  if (path.empty()) {
    return false;
  }
  // Never between a crash and its report: if decoding this image is what took
  // the device down, the built-in logo is the way out of a boot loop.
  if (HalSystem::isRebootFromPanic()) {
    LOG_INF("BOOT", "Rebooted from a panic: skipping the custom boot image");
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("BOOT", path, file)) {
    LOG_ERR("BOOT", "Boot image missing, using the built-in logo: %s", path.c_str());
    return false;
  }
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    LOG_ERR("BOOT", "Boot image is not a usable BMP, using the built-in logo: %s", path.c_str());
    file.close();
    return false;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int imageWidth = bitmap.getWidth();
  const int imageHeight = bitmap.getHeight();
  int x = (pageWidth - imageWidth) / 2;
  int y = (pageHeight - imageHeight) / 2;
  if (imageWidth > pageWidth || imageHeight > pageHeight) {
    // drawBitmap scales a larger image down to fit; centre the scaled result.
    // Cross-multiplied so the fit stays in integers.
    if (static_cast<int64_t>(imageWidth) * pageHeight > static_cast<int64_t>(imageHeight) * pageWidth) {
      x = 0;
      y = (pageHeight - static_cast<int>(static_cast<int64_t>(imageHeight) * pageWidth / imageWidth)) / 2;
    } else {
      x = (pageWidth - static_cast<int>(static_cast<int64_t>(imageWidth) * pageHeight / imageHeight)) / 2;
      y = 0;
    }
  }

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
  file.close();
  return true;
}

void drawDefaultBootScreen(GfxRenderer& renderer) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
}

}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  if (!drawCustomBootImage(renderer)) {
    drawDefaultBootScreen(renderer);
  }
  renderer.displayBuffer();
}
