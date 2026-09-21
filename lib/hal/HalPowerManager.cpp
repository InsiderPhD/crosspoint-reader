#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/gpio.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/rtc_io.h>
#endif
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp_system.h>  // esp_restart()
#include <soc/gpio_num.h>

#include <cassert>

#include "HalFrontlight.h"
#include "HalGPIO.h"
#include "HalStorage.h"

// GPIO13 (SPIWP) drives a battery-protection MOSFET on the X4 hardware.
// HIGH (default) = battery connected; LOW = battery disconnected.
// Pulling it LOW before deep sleep cuts quiescent draw from ~3-4mA to near zero.
static constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;

// Records a rejected deep-sleep entry across the esp_restart() that follows it,
// so the next boot can report it via takeAbortedSleepInfo(). RTC_NOINIT_ATTR
// survives esp_restart() (a warm reset, same RTC-memory domain) but holds
// garbage on a cold boot, hence the magic guard — same pattern as main.cpp's
// silentRebootMagic.
static constexpr uint32_t ABORTED_SLEEP_MAGIC = 0x41424f52;  // 'ABOR'

struct AbortedSleepRecord {
  bool aborted;
  int wakeupCause;
  int wakePinLevel;
};

static RTC_NOINIT_ATTR uint32_t abortedSleepMagic;
static RTC_NOINIT_ATTR AbortedSleepRecord abortedSleepRecord;

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

#if FREEINK_DEVICE_X4PRO
  // No battery-disconnect latch on this board (GPIO13 is the display CS line —
  // see the X3/X4 branch below), so every gated peripheral must be cut
  // individually or it stays powered all through deep sleep: the GT911 touch
  // rail (GPIO2, active-LOW) and the SD card enable (GPIO5, active-LOW) alone
  // are milliamps of standby drain. powerDownRailsForSleep() drives each
  // assigned rail enable to its OFF level and latches it with gpio_hold_en();
  // the boot/init paths (holdPowerRails, InputManager, SdmmcBlockDevice,
  // EpdBus) all gpio_hold_dis before re-driving those pins after the wake
  // reset. Cutting the touch rail forfeits touch-to-wake — fine here, wake is
  // the power button via ext1 below.
  //
  // The SD card goes first. Cutting its enable alone does not unpower it: the
  // SDMMC host leaves CLK/CMD/D0 idling high on their internal pull-ups, which
  // back-feed the card's VDD through its bus pins for the whole sleep (upstream
  // measured 3.3 V on VDD with the enable driven off). shutdown() unmounts, stops
  // the host and floats those pads; the isolation below keeps them floating.
  Storage.shutdown();
  freeink::PowerManager::powerDownRailsForSleep();

  // The frontlight driver sits behind the master rail held up below, so its PWM
  // pads must be latched off too or they float high-Z for the whole sleep. Done
  // here rather than in enterDeepSleep() so every sleep entry is covered,
  // including the AfterUSBPower re-sleep in setup(), which runs after the saved
  // brightness has already been applied.
  halFrontlight.parkForDeepSleep();

  // The master peripheral rail (power.latch0, GPIO1) stays ON but must be
  // latched: an unheld output goes high-Z in deep sleep and would leave the
  // rail switch floating half-on. Keeping it up costs only µA — behind it sit
  // the BM8563 RTC, which keeps the wall clock through sleep, and the panel,
  // already commanded into DSLP (display.deepSleep()) with its RESET held HIGH
  // by powerDownRailsForSleep() so it cannot drift back out.
  const int8_t latch0 = BoardConfig::ACTIVE.power.latch0;
  if (latch0 >= 0) {
    const auto latchGpio = static_cast<gpio_num_t>(latch0);
    gpio_hold_dis(latchGpio);
    gpio_set_direction(latchGpio, GPIO_MODE_OUTPUT);
    gpio_set_level(latchGpio, 1);
    gpio_hold_en(latchGpio);
  }

  // Isolate floating GPIOs and make the rail holds persist through deep sleep.
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
#else
  // Drive GPIO13 (SPIWP) LOW to disconnect the battery via the hardware protection circuit.
  // On battery power this triggers a full MCU shutdown; the power button is a hardware wake trigger.
  // On USB power, the software GPIO wakeup below still applies.
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
  // The pinMode() pull-up above does NOT survive into deep sleep here. ext1 routes
  // the pad to the RTC mux, and the S3's RTC pad pull is a separate register from
  // the digital one (SOC_GPIO_SUPPORT_RTC_INDEPENDENT), so without this the wake
  // pin floats all night. A floating ANY_LOW pin wakes the device at random, and
  // since a power-button wake now boots straight through (see setup()), each one
  // is a full boot with the frontlight restored, awake until the sleep timeout,
  // then back to the sleep screen — invisible by morning. RTC_PERIPH is powered
  // down in sleep, so IDF latches this pull with the pad hold on sleep entry.
  const auto wakePin = static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN);
  rtc_gpio_pulldown_dis(wakePin);
  rtc_gpio_pullup_en(wakePin);
  esp_sleep_enable_ext1_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
  // Enter Deep Sleep.
  //
  // Called through a volatile function pointer, NOT directly: IDF declares
  // esp_deep_sleep_start() __attribute__((noreturn)) (esp_sleep.h), so a direct
  // call lets the compiler prove the recovery below is unreachable and delete
  // it — the guard would compile away to nothing and we would never know. The
  // indirect call hides the attribute from the optimiser. (freeink-sdk d37a158
  // calls it directly and so may lose its copy of this guard to the same
  // optimisation.)
  static void (*volatile deepSleepStart)() = &esp_deep_sleep_start;
  deepSleepStart();

  // esp_deep_sleep_start() does not return in normal operation. Reaching this
  // line means the SoC rejected sleep entry — the commonest cause being a wake
  // source that is already asserted (the ext1 pin sitting LOW). Falling out of
  // here used to return into loop(), which leaves the device running at full
  // clock behind a sleep screen, deaf to everything but a fresh power press,
  // and — because lastActivityTime is never reset — re-attempting the whole
  // sleep sequence every iteration. From the outside that is indistinguishable
  // from a battery-drain bug, which is exactly what made previous drain hunts
  // unfalsifiable. Record the abort in RTC memory (RTC_NOINIT_ATTR survives the
  // warm reset below) and restart properly, so the next boot can say so.
  // Ported from freeink-sdk d37a158.
  abortedSleepRecord.aborted = true;
  abortedSleepRecord.wakeupCause = static_cast<int>(esp_sleep_get_wakeup_cause());
  abortedSleepRecord.wakePinLevel = digitalRead(InputManager::POWER_BUTTON_PIN);
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  esp_restart();  // [[noreturn]], which satisfies this function's own contract
}

HalPowerManager::AbortedSleepInfo HalPowerManager::takeAbortedSleepInfo() {
  AbortedSleepInfo info;
  if (abortedSleepMagic == ABORTED_SLEEP_MAGIC && abortedSleepRecord.aborted) {
    info.aborted = true;
    info.wakeupCause = abortedSleepRecord.wakeupCause;
    info.wakePinLevel = abortedSleepRecord.wakePinLevel;
  }
  // Clear so a stale record isn't reported again on a later boot, and stamp the
  // magic so an uninitialised cold-boot read reports false rather than trusting
  // a garbage `aborted` bit.
  abortedSleepRecord.aborted = false;
  abortedSleepMagic = ABORTED_SLEEP_MAGIC;
  return info;
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
