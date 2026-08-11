#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <soc/gpio_num.h>

#include <cassert>

#include "HalGPIO.h"

// GPIO13 (SPIWP) drives a battery-protection MOSFET on the X4 hardware.
// HIGH (default) = battery connected; LOW = battery disconnected.
// Pulling it LOW before deep sleep cuts quiescent draw from ~3-4mA to near zero.
static constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;

HalPowerManager powerManager;  // Singleton instance

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
#if !FREEINK_DEVICE_X4PRO
  } else {
    // X4: GPIO0 is the battery ADC input.
    // NOT on the X4 Pro, where GPIO0 is the Left nav button AND the boot strap.
    // InputManager has already configured it as INPUT_PULLUP; re-declaring it as a
    // plain INPUT here would drop the pull-up and leave the button floating — and
    // that button is the strap used to reach the ROM bootloader for recovery.
    pinMode(BAT_GPIO0, INPUT);
#endif
  }
  // Mutex first: setPowerSaving() uses `normalFreq > 0` as its "begin() has run"
  // gate and takes modeMutex unconditionally after that check.
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
  normalFreq = getCpuFrequencyMhz();
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // The BLE controller's timing budget assumes >=80MHz (Espressif's DFS floor
  // with BT enabled). At LOW_POWER_FREQ the controller drops advertisement
  // reports and misses connection events — and scan start/stop at 10MHz has
  // hard-frozen the device with no panic output (observed 2026-07-30, right
  // after the inactivity disconnect dropped back to low-power mid-scan-cycle).
  const int lowFreq =
      esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE ? LOW_POWER_FREQ : BLE_LOW_POWER_FREQ;

  // Serialised, including the setCpuFrequencyMhz() call itself. This used to run
  // lock-free ("a slightly stale currentLockMode doesn't matter"), which held on
  // the single-core C3. It does not hold on the dual-core X4 Pro: a page turn
  // coming out of idle has the loop task on core 0 (main.cpp, on user input) and
  // the render task on core 1 (ActivityManager's HalPowerManager::Lock) both call
  // this within the same millisecond, both read isLowPower == true, and both enter
  // setCpuFrequencyMhz(). That reconfigures the PLL and walks Arduino's
  // APB-change callback list, neither of which is cross-core safe.
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const LockMode mode = currentLockMode;

  if (mode == None && enabled && (!isLowPower || appliedLowFreq != lowFreq)) {
    LOG_DBG("PWR", "Going to low-power mode (%d MHz)", lowFreq);
    if (setCpuFrequencyMhz(lowFreq)) {
      isLowPower = true;
      appliedLowFreq = lowFreq;
    } else {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", lowFreq);
    }

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (setCpuFrequencyMhz(normalFreq)) {
      isLowPower = false;
    } else {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
    }
  }
  // Otherwise, no change needed
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#if !FREEINK_DEVICE_X4PRO
  // Drive GPIO13 (SPIWP) LOW to disconnect the battery via the hardware protection circuit.
  // On battery power this triggers a full MCU shutdown; the power button is a hardware wake trigger.
  // On USB power, the software GPIO wakeup below still applies.
  // X4 PRO: deliberately skipped. This latch is an X3/X4 board feature, and on the
  // X4 Pro GPIO13 is the display CS line — driving it low and holding it through
  // deep sleep would clamp chip select, not disconnect the battery.
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);

  // Isolate GPIOs and latch GPIO13 LOW through the deep sleep cycle so the battery stays disconnected.
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
#endif
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);

  // Arm the wakeup trigger *after* the button is released (effective on USB power).
  // The RISC-V parts (C3) expose a dedicated GPIO deep-sleep wakeup; the Xtensa S3
  // has no such API and must go through the RTC's ext1 path instead. The X4 Pro's
  // power button (GPIO3) is RTC-capable, which ext1 requires.
#if CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  // Default-construct: BatteryMonitor takes its ADC pin, divider and charge-status
  // pin from the active board profile. Do NOT pass BAT_GPIO0 here — that is the
  // C3's battery ADC, but GPIO0 on the X4 Pro is the Left nav button and the boot
  // strap, so an explicit pin would sample (and fight) the button. The X4 Pro reads
  // its charge from the CW2017 I2C gauge above and never reaches this path.
  static const BatteryMonitor battery;

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
