#pragma once

#include <BluetoothHIDManager.h>
#include <esp_wifi.h>

// Holds WiFi modem sleep off for the lifetime of a transfer and puts the
// previous mode back afterwards.
//
// Why: in WIFI_PS_MIN_MODEM the radio sleeps between DTIM beacons, so every
// burst of an inbound TCP stream waits for the next beacon before the AP
// releases it. Throughput on a bulk download drops accordingly, and an idle
// link can miss the first SYN-ACK outright. No allocation, two bytes of state.
//
// Nesting is safe: an inner guard records WIFI_PS_NONE as "previous" and
// restores exactly that, leaving the outer guard to restore the real mode.
//
// Skipped while BLE is up. That only happens on boards with software
// coexistence (CROSSPOINT_BLE_EXCLUSIVE=0, the ESP32-S3 ones): the two radios
// share one antenna by time-slicing, and coexistence relies on WiFi modem sleep
// to hand the BLE stack its slots. The C3 boards tear BLE down before WiFi
// comes up, so the guard is always live there.
class WifiPowerSaveGuard final {
 public:
  WifiPowerSaveGuard() {
    if (BluetoothHIDManager::getInstance().isEnabled()) return;
    wifi_ps_type_t current = WIFI_PS_MIN_MODEM;
    // Fails when WiFi is not initialised; then there is nothing to change or restore.
    if (esp_wifi_get_ps(&current) != ESP_OK || current == WIFI_PS_NONE) return;
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return;
    previousMode_ = current;
    restore_ = true;
  }

  ~WifiPowerSaveGuard() {
    if (restore_) esp_wifi_set_ps(previousMode_);
  }

  WifiPowerSaveGuard(const WifiPowerSaveGuard&) = delete;
  WifiPowerSaveGuard& operator=(const WifiPowerSaveGuard&) = delete;

 private:
  wifi_ps_type_t previousMode_ = WIFI_PS_MIN_MODEM;
  bool restore_ = false;
};
