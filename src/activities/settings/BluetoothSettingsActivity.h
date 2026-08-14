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
  // MAP_BACK / MAP_FORWARD are the button-mapping wizard: each step captures the
  // same button twice (a one-button "toggle" remote alternates codes per press,
  // and the double capture is what exposes it).
  enum class ViewMode {
    MAIN_MENU,
    SCANNING,
    DEVICE_LIST,
    CONNECTING,
    CONNECTED,
    CONNECT_FAILED,
    MAP_BACK,
    MAP_FORWARD
  };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";

  // Device being connected to, for the connecting/connected/failed screens.
  std::string selectedDeviceName;
  std::string connectionError;
  // True while the connecting/connected/failed screens belong to a bonded-remote
  // reconnect from the main menu (no scan ran): the header skips the device
  // count and a failed result retries the bond instead of showing the list.
  bool connectingBonded = false;

  // Live remote test box on the main menu. The manager timestamps every report
  // it receives; we watch that value for changes so each press is counted once.
  unsigned long lastSeenRemoteInputMs = 0;
  unsigned long lastRemotePressAtMs = 0;
  uint32_t remotePressCount = 0;

  // Button-mapping wizard state. Each step locks a signature after two matching
  // presses; the back step's result is held here until the forward step confirms.
  unsigned long lastSeenMapPressMs = 0;
  uint8_t mapStepPresses = 0;  // presses captured in the current step (0 or 1)
  uint8_t mapStepSigIndex = 0;
  uint8_t mapStepSigValue = 0;
  uint8_t mapBackSigIndex = 0;
  uint8_t mapBackSigValue = 0;

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
  // Main-menu Reconnect row: connect to the bonded remote without a scan.
  void reconnectBonded();
  void disconnectAll();

  // Button-mapping wizard (MAP_BACK / MAP_FORWARD view modes).
  void beginButtonMapping();
  void handleMappingInput();
  // Polls the manager for a captured press and advances the wizard.
  void pollMappingPress();
  // Leaves the wizard for the main menu. Saves the learned pair, clears the
  // mapping (indistinguishable buttons), or keeps the old one (cancel).
  enum class MappingOutcome { SAVED, CLEARED, CANCELLED };
  void endButtonMapping(MappingOutcome outcome);
  void renderMapping() const;

  std::string getSignalStrengthIndicator(const int32_t rssi) const;
};
