#include "BookLoadingFeedback.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "components/UITheme.h"

bool BookLoadingFeedback::showIfDue(GfxRenderer& renderer) {
  const uint32_t now = millis();
  if (!isDue(now)) return false;
  finish();
  LOG_DBG("READER", "Slow book open: Loading popup after %lu ms", static_cast<unsigned long>(now - startedAt));
  // On a blank page rather than over whatever the buffer holds: that may be the
  // previous screen already inverted for dark mode (drawPopup() would invert it
  // back), or bytes a framebuffer loan left behind.
  renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_LOADING));
  return true;
}
