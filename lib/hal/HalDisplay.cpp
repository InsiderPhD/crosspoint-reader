#include <HalDisplay.h>
#include <HalGPIO.h>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  // Request resync after specific wakeup events to ensure clean display state.
  // Skip when seamless=true so the current screen content is preserved.
  if (!seamless) {
    const auto wakeupReason = gpio.getWakeupReason();
    if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
        wakeupReason == HalGPIO::WakeupReason::Other) {
      einkDisplay.requestResync();
    }
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::applyRefreshPolicy(HalDisplay::RefreshMode mode) {
  if (mode != RefreshMode::FAST_REFRESH) {
    // FULL flashes natively on every driver. HALF is the readers' periodic
    // scrub, but the UC8179 (X4 Pro) and UC8279 X4 drivers route any non-Full
    // mode down the fast partial path, so without the resync the "scrub"
    // never actually cleans accumulated residue there. Drivers whose Half
    // waveform already scrubs (UC8253/SSD1677) treat the resync as the same
    // flash or a no-op, so arming it unconditionally is safe.
    if (mode == RefreshMode::HALF_REFRESH) einkDisplay.requestResync(1);
    _fastRefreshStreak = 0;
    return;
  }
  // Fast/partial waveforms are not DC-balanced; menus and browsers only ever
  // request FAST, so a long UI session accumulates residual charge that shows
  // up as speckle/noise. Promote every Nth consecutive fast paint to a full
  // GC flash of the frame being displayed. Reader page turns reset the streak
  // via their HALF scrub above, so this only fires on fast-only streaks.
  if (++_fastRefreshStreak >= FAST_REFRESH_SCRUB_LIMIT) {
    einkDisplay.requestResync(1);
    _fastRefreshStreak = 0;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  applyRefreshPolicy(mode);
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  _lastRefreshMs = millis();
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  applyRefreshPolicy(mode);
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  _lastRefreshMs = millis();
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

void HalDisplay::powerOffIdle() { einkDisplay.powerOffIdle(); }

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::releaseBuffers() { einkDisplay.releaseBuffers(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  einkDisplay.displayGrayBuffer(turnOffScreen);
  _lastRefreshMs = millis();
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
