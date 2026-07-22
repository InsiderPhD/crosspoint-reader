#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>

#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BluetoothSettingsActivity : public Activity {
 private:
  // The pairing flow mirrors WifiSelectionActivity state-for-state so both
  // radios feel the same: a dedicated scanning screen you can back out of, a
  // plain list of what was found, and an explicit connected/failed result —
  // rather than folding actions into the list as pseudo-rows.
  enum class ViewMode { MAIN_MENU, SCANNING, DEVICE_LIST, CONNECTING, CONNECTED, CONNECT_FAILED };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";

  // Device being connected to, for the connecting/connected/failed screens.
  std::string selectedDeviceName;
  std::string connectionError;

  // Live remote test box on the main menu. The manager timestamps every report
  // it receives; we watch that value for changes so each press is counted once.
  unsigned long lastSeenRemoteInputMs = 0;
  unsigned long lastRemotePressAtMs = 0;
  uint32_t remotePressCount = 0;

  static constexpr uint32_t SCAN_DURATION_MS = 10000;

 public:
  explicit BluetoothSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BluetoothSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool keepsBluetoothActive() const override { return true; }

 private:
  void handleMainMenuInput();
  void handleScanningInput();
  void handleDeviceListInput();
  void handleResultInput();

  void renderMainMenu();
  // "Press a button on your remote" box, so a user can confirm the remote works
  // without leaving Settings and opening a book.
  void renderRemoteTestBox(int boxTop, int boxHeight) const;
  // Polls the manager for a new remote report; returns true if one arrived.
  bool pollRemoteTest();
  // Header + subheader shared by every screen in the pairing flow, matching the
  // way WifiSelectionActivity::render() frames its own states.
  void drawScanHeader() const;
  void renderScanning() const;
  void renderDeviceList() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderConnectFailed() const;

  void beginScan();
  void connectToSelected();
  void disconnectAll();

  std::string getSignalStrengthIndicator(const int32_t rssi) const;
};
