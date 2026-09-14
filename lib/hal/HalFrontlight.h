#pragma once

#include <FrontlightManager.h>

#include <cstdint>

class HalFrontlight;
extern HalFrontlight halFrontlight;  // Singleton

// HAL wrapper around the SDK FrontlightManager (X4 Pro: warm/cool PWM pair on
// GPIO8/GPIO9). Inert on boards without a frontlight, so every method is safe
// to call unconditionally.
class HalFrontlight {
  FrontlightManager _manager;

 public:
  // Bring up the PWM channel(s). Call once at boot, before apply().
  void begin();

  // True when this board wires a frontlight at all.
  bool present() const { return _manager.present(); }
  // True when a second warm channel exists, so warmth is adjustable.
  bool hasWarmth() const { return _manager.hasColorTemperature(); }

  // Drive the light: brightness 0-100 (0 = off), warmth 0-100 (0 = fully cool,
  // 100 = fully warm; total brightness stays constant across the mix).
  void apply(uint8_t brightnessPercent, uint8_t warmthPercent);

  // Force the light off (sleep/shutdown path). Persisted settings are untouched.
  void off();

  // Turn the light off and latch its PWM pads at the off level so they cannot
  // float through deep sleep. Call only as the last step before
  // esp_deep_sleep_start(); begin() releases the latch on the next boot.
  void parkForDeepSleep();

 private:
  bool _begun = false;
};
