#include "SleepActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/MoonIcon.h"
#include "sleeptheme/SleepThemeRenderer.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool renderSeamless =
      SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_ALWAYS ||
      (fromTimeout &&
       SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_AFTER_TIMEOUT);

  if (renderSeamless) {
    return renderLastScreenSleepScreen();
  }

  SleepThemeRenderer::ThemeInfo theme;
  const bool haveTheme = SleepThemeRenderer::inspect(SETTINGS.sleepTheme, theme);

  // An overlay paints onto the screen that is still in the framebuffer, so it
  // has to run before the popup below draws over it.
  if (haveTheme && theme.overlay) {
    return renderOverlayThemeSleepScreen();
  }

  // "Full refresh before sleep": scrub the panel before the sleep image goes
  // up, showing "Entering sleep" on a full GC flash so residue accumulated
  // while reading (fast/partial waveforms are not DC-balanced) can't ghost
  // through the sleep screen. Off by default: it cost ~1.6 s of a ~3.8 s sleep
  // on the X4 Pro, and the sleep image's own FULL_REFRESH still scrubs (a
  // greyscale theme clears to white on a FULL first, so it never needs this).
  // Off, there is no popup either: the page stays up for the half second the
  // theme takes to draw, then the sleep screen replaces it.
  if (SETTINGS.sleepFullRefresh && !(haveTheme && theme.greyscale)) {
    display.requestResync(1);
    // Show popup with reader orientation only when going to sleep from reader
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    const unsigned long popupStart = millis();
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    LOG_INF("SLP", "Popup shown in %lu ms", millis() - popupStart);
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.sleepScreenOrientation);
  if (haveTheme) {
    return renderThemeSleepScreen(theme.greyscale);
  }
  LOG_INF("SLP", "No usable sleep theme '%s'; using the default sleep screen", SETTINGS.sleepTheme);
  renderDefaultSleepScreen();
}

void SleepActivity::renderOverlayThemeSleepScreen() const {
  // Dark mode screens are inverted in the framebuffer just before they are
  // shown, so the page being kept is white-on-black. Flip it to its natural
  // polarity, paint, and flip the result back: the overlay then follows dark
  // mode like the rest of the UI, and black art stays visible instead of
  // vanishing into a black page.
  if (SETTINGS.darkMode) renderer.invertScreen();
  const bool drawn = SleepThemeRenderer::draw(renderer, SETTINGS.sleepTheme);
  if (SETTINGS.darkMode) renderer.invertScreen();
  if (!drawn) {
    LOG_INF("SLP", "Overlay theme '%s' not drawn; using the default sleep screen", SETTINGS.sleepTheme);
    ReaderUtils::applyOrientation(renderer, SETTINGS.sleepScreenOrientation);
    return renderDefaultSleepScreen();
  }

  // A full waveform, like every other generated sleep screen: this image has
  // to survive the rail collapsing on battery (see renderDefaultSleepScreen).
  // That is also why this is not the flash-free seamless path, and the wake
  // shows the boot screen as usual.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
}

void SleepActivity::renderThemeSleepScreen(const bool greyscale) const {
  if (greyscale) {
    // The grayscale LUT only nudges particles, so the BW base under it must
    // not be a strong waveform. A HALF base no longer works: HalDisplay arms a
    // resync on every HALF, which on the X3 is a full-sync waveform that sets
    // the pixels too firmly for the grey drive to move, and images came out
    // in harsh, wrong tones. Scrub to white on a FULL first so the base paint
    // below (a differential) doesn't ghost the screen underneath.
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  // Drawn exactly as designed: no dark-mode invert (themes are their own art).
  SleepThemeRenderer::GreyPlan grey;
  const unsigned long drawStart = millis();
  const bool drawn = SleepThemeRenderer::draw(renderer, SETTINGS.sleepTheme, greyscale ? &grey : nullptr);
  LOG_INF("SLP", "Theme %s drawn in %lu ms", SETTINGS.sleepTheme, millis() - drawStart);
  if (!drawn) {
    LOG_INF("SLP", "Theme '%s' not drawn; using the default sleep screen", SETTINGS.sleepTheme);
    ReaderUtils::applyOrientation(renderer, SETTINGS.sleepScreenOrientation);
    return renderDefaultSleepScreen();
  }

  if (grey.count == 0) {
    // FULL_REFRESH so the image survives the rail collapsing on battery (see
    // renderDefaultSleepScreen).
    const unsigned long refreshStart = millis();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
    LOG_INF("SLP", "Sleep screen refreshed in %lu ms", millis() - refreshStart);
    return;
  }

  // The BW paint is the base; the grey passes then hold only the grey images,
  // so displayGrayBuffer leaves every other pixel as the base drew it.
  //
  // The X3's UC8253 has a dedicated grey-base waveform (the OEM "AA-pre-BW(mid)"
  // bank): a differential update that gives the changed pixels the calibrated
  // strong drives the gc nudge bank was tuned against. A plain FAST base is the
  // short "turbo" bank, whose blacks are too weak to start from, and the grey
  // nudge then lightens them further: every image came out washed out. The
  // other panels have no such waveform and their base stays a FAST paint.
  if (!gpio.deviceIsX3() ||
      !renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Overlay, HalDisplay::FAST_REFRESH)) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  SleepThemeRenderer::drawGreyImages(renderer, grey);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  SleepThemeRenderer::drawGreyImages(renderer, grey);
  renderer.copyGrayscaleMsbBuffers();

  // displayGrayBuffer powers the rails off.
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
}

void SleepActivity::renderDefaultSleepScreen() const {
  // The store's "light", "dark", "stats" and "stats-dark" themes are this
  // screen, so they work before they are downloaded; anything else falls back
  // to light.
  SleepThemeRenderer::drawFallback(renderer, SETTINGS.sleepTheme);

  // Terminate on a FULL_REFRESH, not a partial one. On battery, deep sleep
  // opens the battery-protection MOSFET (HalPowerManager::startDeepSleep) and
  // the panel rail collapses entirely. A partial waveform (HALF/FAST) leaves
  // the e-ink particles in a metastable state that relies on a powered
  // controller to hold it; once VCC drops, the particles relax and the old
  // image ghosts back over a few seconds. A full waveform commits every pixel
  // to its bistable rail so the image survives power removal. powerOffAfter
  // still collapses the rails cleanly as the terminating phase of the refresh.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH, /*powerOffAfter=*/true);
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  // Deliberately stays on HALF_REFRESH (partial waveform): the whole point of
  // seamless sleep is to keep the last page on screen WITHOUT a flash, and a
  // FULL_REFRESH inverts the panel. The tradeoff is that on battery the
  // metastable pixels can ghost slightly as the rail collapses (see
  // renderDefaultSleepScreen). Seamless users opt into flash-free over
  // ghost-free; do not "fix" this to FULL_REFRESH.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH, /*powerOffAfter=*/true);
}
