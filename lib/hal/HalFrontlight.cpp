#include "HalFrontlight.h"

#include <Logging.h>
#include <driver/gpio.h>

HalFrontlight halFrontlight;  // Singleton instance

void HalFrontlight::begin() {
  if (!_manager.present()) {
    return;
  }
  // parkForDeepSleep() latched the pads with gpio_hold_en(), and that hold
  // survives both deep sleep and the wake reset. A held pad silently ignores the
  // LEDC drive begin() is about to attach, so the light would stay dark until a
  // power cycle. Release unconditionally: nothing survives the reset to say
  // whether we parked, and gpio_hold_dis() on an unheld pad is a no-op.
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin >= 0) gpio_hold_dis(static_cast<gpio_num_t>(pin));
  }
  _manager.begin();
  _begun = true;
  LOG_INF("FLIGHT", "Frontlight ready (%s)", _manager.hasColorTemperature() ? "warm/cool" : "single channel");
}

void HalFrontlight::apply(uint8_t brightnessPercent, uint8_t warmthPercent) {
  if (!_manager.present()) {
    return;
  }
  _manager.setColorTemperature(warmthPercent);
  _manager.setBrightness(brightnessPercent);
}

void HalFrontlight::off() {
  if (!_manager.present()) {
    return;
  }
  _manager.off();
}

void HalFrontlight::parkForDeepSleep() {
  if (!_manager.present()) {
    return;
  }
  // Duty 0 is not enough on its own. The LEDC peripheral stops driving in deep
  // sleep and esp_sleep_config_gpio_isolate() leaves every unheld pad high-Z, so
  // the LED driver's PWM inputs float — while the X4 Pro holds its master rail
  // (GPIO1) up through sleep, keeping that driver powered. Measured 2026-09-14:
  // 10% battery overnight asleep with the light at 1%.
  //
  // The SDK's FrontlightManager::park() does this, but only under
  // FREEINK_FRONTLIGHT_LS, which also moves LEDC onto RC_FAST — and RC_FAST
  // cannot reach this board's 25 kHz / 10-bit PWM (17.5 MHz / 25 kHz = 700 steps),
  // so enabling that flag would leave the light unattachable. Hence a local park.
  if (_begun) {
    _manager.off();
  }
  const auto& fl = BoardConfig::ACTIVE.frontlight;
  const uint8_t offLevel = fl.activeHigh ? LOW : HIGH;
  for (const int8_t pin : {fl.gpio, fl.gpioWarm}) {
    if (pin < 0) continue;
    const auto gpioNum = static_cast<gpio_num_t>(pin);
    if (_begun) {
      ledcDetach(pin);  // hand the pad back from the LEDC signal matrix
    }
    gpio_hold_dis(gpioNum);  // a pad left held by an earlier cycle would ignore the drive
    pinMode(pin, OUTPUT);
    digitalWrite(pin, offLevel);
    // Held through deep sleep by the caller's gpio_deep_sleep_hold_en().
    gpio_hold_en(gpioNum);
  }
  LOG_DBG("FLIGHT", "Frontlight pads parked for deep sleep");
}
