#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight halFrontlight;  // Singleton instance

void HalFrontlight::begin() {
  if (!_manager.present()) {
    return;
  }
  _manager.begin();
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
