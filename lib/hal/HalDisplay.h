#pragma once
#include <Arduino.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  // Which panel controller the boot-time bus probe found. Lets a caller gate a
  // behaviour on the actual silicon rather than on the device profile, which
  // matters because one profile can ship with more than one panel.
  using Controller = BoardConfig::DisplayController;
  Controller getController() const;

  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  using GrayscaleBase = freeink::GrayscaleBase;
  using GrayscaleEncoding = freeink::GrayscaleEncoding;

  // What this panel supports for a given grayscale mode. Querying selects
  // nothing; displayGrayscaleBase() is what commits to a mode.
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const;

  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  // Initialize the display hardware and driver
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Power management
  void deepSleep();
  // Drop the panel's analog rails while the UI is idle, keeping the controller's
  // partial-refresh baseline. No-op if they are already down. Must be called with
  // the render lock held — it drives the same SPI bus as the render task.
  void powerOffIdle();

  // Arm a full DC-balanced (GC) flash for the next refresh regardless of the
  // mode the caller requests. Used to scrub accumulated partial-refresh
  // residue: pair with a normal activity repaint so overlays (AA text,
  // grayscale images) re-render through their usual path instead of being
  // flattened to the B/W framebuffer.
  void requestResync(uint8_t settlePasses = 1) { einkDisplay.requestResync(settlePasses); }
  // Drive the panel's border ring black (dark mode) on panels that support it
  // (Picco SSD1677); applied by the next full refresh. Cheap to call every loop.
  void setDarkBorder(bool dark) { einkDisplay.setDarkBorder(dark); }

  // Defer the FAST-streak GC promotion (see applyRefreshPolicy) while a burst-
  // input screen is up: on the keyboard it landed a >1 s flash mid-word every
  // FAST_REFRESH_SCRUB_LIMIT keystrokes. The streak keeps counting, so the
  // first FAST paint after resuming (the screen the keyboard returns to) takes
  // the owed scrub instead.
  void setFastScrubSuspended(bool suspended) { _fastScrubSuspended = suspended; }

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Hand the framebuffer's ~48KB back to the heap. E-ink is bistable, so the
  // panel keeps showing its last refreshed image with no buffer behind it —
  // which makes the memory pure dead weight for any session that paints once
  // and then stops. No display operation is legal afterwards; there is
  // deliberately no realloc wrapper, because reallocating relocates the 48KB
  // and fragments a PSRAM-less heap. The only way back is a reboot, so callers
  // must be sessions that restart on exit (see CrossPointWebServerActivity).
  void releaseBuffers();

  // Drain any deferred refresh (async fire / split displayStart+displayFinish)
  // so the panel pipeline is no longer reading the framebuffer. No-op when
  // nothing is pending.
  void waitRefreshComplete();

  // Milliseconds since the last refresh op (displayBuffer / refreshDisplay /
  // displayGrayBuffer). Drives the idle rail collapse: a panel left powered
  // under a static screen develops image drift, so the main loop powers it
  // off once no refresh has happened for a few seconds — independent of the
  // activity/sleep timers, which preventAutoSleep() activities suppress.
  // Every call that drives the glass must reset this: main.cpp's static-screen
  // maintenance (S3) flashes the screen once it passes 5 minutes, so a refresh
  // path that forgets it gets a full flash mid-reading.
  unsigned long msSinceLastRefresh() const { return millis() - _lastRefreshMs; }

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

  // Arm the panel for `mode` and display the B/W frame as its base. Returns
  // false when the panel would not take the mode, in which case the caller must
  // fall back to an ordinary display and skip the grayscale pass entirely.
  bool displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  // Absolute 16-level image on panels that have it (Picco SE0400NQW47): 4 bpp,
  // 0 = white .. 15 = darkest, panel-native rows of getDisplayWidth()/2 bytes.
  // The framebuffer is untouched; the next display call clears the image.
  bool supportsGray16Image() const { return einkDisplay.supportsGray16Image(); }
  // fast: turn from a 16-level image already up without the full clears.
  // mode: FAST_REFRESH = the panel's fast turn (Picco: dissolve from a 16-level
  // image already up), HALF_REFRESH = clean draw.
  bool displayGray16Image(const uint8_t* levels, RefreshMode mode = HALF_REFRESH, bool turnOffScreen = true);
  bool setGray16Turn(uint8_t scrubEvery) { return einkDisplay.setGray16Turn(scrubEvery); }
  // Measured L* x10 per level (0 = white .. 15 = darkest) of a clean 16-level
  // draw on this panel, or nullptr when the driver has none.
  const int16_t* gray16LevelLstar() const { return einkDisplay.gray16LevelLstar(); }
  // 16-level text: the levels its base draws black, and the deep level pulled
  // back inside the turn (-1: none). Both follow the driver's paths.
  uint16_t gray16DeepLevels() const { return einkDisplay.gray16DeepLevels(); }
  int gray16TextSoftLevel() const { return einkDisplay.gray16TextSoftLevel(); }
  // A 16-level image's owed drive across deep sleep: export before sleeping,
  // import after begin() on wake; the next display call gives it back.
  size_t gray16StateSize() const { return einkDisplay.gray16StateSize(); }
  size_t exportGray16State(uint8_t* out, size_t len) const { return einkDisplay.exportGray16State(out, len); }
  bool importGray16State(const uint8_t* in, size_t len) { return einkDisplay.importGray16State(in, len); }
  // 16-level text: a readable B/W base (the dark levels) with the reader's
  // B/W turn, then the calibrated 16-level passes.
  bool beginGray16Text(const uint8_t* base, const uint8_t* soft = nullptr) {
    const bool ok = einkDisplay.beginGray16Text(base, soft);
    if (ok) _lastRefreshMs = millis();
    return ok;
  }
  bool setGray16TextSoftPull(uint8_t frames) { return einkDisplay.setGray16TextSoftPull(frames); }
  bool finishGray16Text(const uint8_t* levels, bool turnOffScreen = true) {
    const bool ok = einkDisplay.finishGray16Text(levels, turnOffScreen);
    if (ok) _lastRefreshMs = millis();
    return ok;
  }
  // Calibration: coarse-black and pull-back doses and the fast-turn erase, in frames.
  bool setGray16Passes(const uint8_t black[3], const uint8_t white[3], uint8_t erase) {
    return einkDisplay.setGray16Passes(black, white, erase);
  }
  // Calibration: paths[l] = {black dose index 0..3, white dose index 0..3, deep 0/1}.
  bool setGray16Levels(const uint8_t (*paths)[3]) { return einkDisplay.setGray16Levels(paths); }

  // Tiled grayscale: stream one band of a plane (lsbPlane selects LSB/MSB RAM)
  // straight to the controller; supportsStripGrayscale() gates the path. See
  // EInkDisplay::writeGrayscalePlaneStrip.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  // Scrub bookkeeping shared by displayBuffer()/refreshDisplay(). HALF is the
  // app's periodic ghost-cleanup mode, but on the UC8179/UC8279 X4-family
  // drivers Half runs the same partial path as FAST, so it is promoted to a
  // one-shot GC resync here. FAST-only streaks (menus/browsers never request
  // anything else) are promoted to a resync every FAST_REFRESH_SCRUB_LIMIT
  // paints so heavy UI use can't accumulate speckle indefinitely.
  void applyRefreshPolicy(RefreshMode mode);

  // Must exceed the reader's largest full-refresh cadence (REFRESH_30) so the
  // user's per-page refresh-frequency setting stays in charge of flash timing
  // while reading; this limit only kicks in for fast-only UI streaks.
  static constexpr uint16_t FAST_REFRESH_SCRUB_LIMIT = 40;

  EInkDisplay einkDisplay;
  unsigned long _lastRefreshMs = 0;
  uint16_t _fastRefreshStreak = 0;
  bool _fastScrubSuspended = false;
};

extern HalDisplay display;
