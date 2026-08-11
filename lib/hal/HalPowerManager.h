#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;
  int appliedLowFreq = 0;  // MHz actually set while isLowPower (10 vs 80 with BLE up)

  // I2C fuel gauge configuration for X3 battery monitoring
  bool _batteryUseI2C = false;                   // True if using I2C fuel gauge (X3), false for ADC (X4)
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if FREEINK_DEVICE_X4PRO
  // X4 PRO (ESP32-S3): 80 MHz is the floor, not 10.
  // Below 80 MHz the CPU leaves the PLL and APB follows the CPU clock, so the
  // USB-OTG CDC console and every APB-clocked peripheral are reclocked by a
  // factor of 8 on the way down and back up. The transition *up* — the one a
  // page turn triggers coming out of idle — reset the device every time with
  // rst:0x8 (TG1WDT_SYS_RST), the interrupt watchdog, with no log output
  // between "Going to low-power mode (10 MHz)" and the ROM banner.
  // This part is dual-core with CONFIG_ESP_INT_WDT_CHECK_CPU1=y, so one core
  // stalled for 300 ms in the clock switch resets the whole system.
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  // Espressif's DFS floor when the BT controller is up: below 80MHz the
  // controller misses advertisement reports and connection events, and scan
  // start/stop at 10MHz can hard-freeze the device with no panic output.
  static constexpr int BLE_LOW_POWER_FREQ = 80;                // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms

  void begin();

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
