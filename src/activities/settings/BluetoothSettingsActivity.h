#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>

#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BluetoothSettingsActivity : public Activity {
 private:
  enum class ViewMode { MAIN_MENU, DEVICE_LIST };

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";
  unsigned long lastScanTime = 0;

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
  void handleDeviceListInput();
  void renderMainMenu();
  void renderDeviceList();
  std::string getSignalStrengthIndicator(const int32_t rssi) const;
};
