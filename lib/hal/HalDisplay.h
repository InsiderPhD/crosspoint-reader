#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
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

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Drain any deferred refresh (async fire / split displayStart+displayFinish)
  // so the panel pipeline is no longer reading the framebuffer. No-op when
  // nothing is pending.
  void waitRefreshComplete();

  // Milliseconds since the last refresh op (displayBuffer / refreshDisplay /
  // displayGrayBuffer). Drives the idle rail collapse: a panel left powered
  // under a static screen develops image drift, so the main loop powers it
  // off once no refresh has happened for a few seconds — independent of the
  // activity/sleep timers, which preventAutoSleep() activities suppress.
  unsigned long msSinceLastRefresh() const { return millis() - _lastRefreshMs; }

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

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
  EInkDisplay einkDisplay;
  unsigned long _lastRefreshMs = 0;
};

extern HalDisplay display;
