#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;
  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() { Activity::onExit(); }

void BluetoothSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (viewMode == ViewMode::DEVICE_LIST) {
      // Return to main menu
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      if (btMgr && btMgr->isScanning()) {
        btMgr->stopScan();
      }
      requestUpdate();
      return;
    } else {
      finish();
      return;
    }
  }

  // Check if scan completed
  if (btMgr && viewMode == ViewMode::DEVICE_LIST && !btMgr->isScanning() && lastScanTime > 0) {
    if (millis() - lastScanTime > 500) {  // Small delay to see final results
      lastScanTime = 0;
      requestUpdate();
    }
  }

  if (viewMode == ViewMode::MAIN_MENU) {
    handleMainMenuInput();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    handleDeviceListInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  constexpr int kMainMenuItemCount = 2;
  constexpr int kToggleBluetoothIndex = 0;
  constexpr int kRemoteIndex = 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (kMainMenuItemCount - 1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < (kMainMenuItemCount - 1)) ? selectedIndex + 1 : 0;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!btMgr) {
      lastError = "BLE not available";
      LOG_ERR("BT", "BLE manager not available");
      requestUpdate();
      return;
    }

    if (selectedIndex == kToggleBluetoothIndex) {
      // Toggle Bluetooth — stack start/stop blocks for up to a second or two.
      {
        RenderLock lock(*this);
        GUI.drawPopup(renderer, btMgr->isEnabled() ? tr(STR_BT_TURNING_OFF) : tr(STR_BT_TURNING_ON));
        renderer.displayBuffer();
      }
      if (btMgr->isEnabled()) {
        LOG_INF("BT", "Disabling Bluetooth...");
        if (btMgr->disable()) {
          lastError = "Bluetooth disabled";
        } else {
          lastError = "Failed to disable";
        }
      } else {
        LOG_INF("BT", "Enabling Bluetooth...");
        if (btMgr->enable()) {
          lastError = "Bluetooth enabled";
        } else {
          lastError = btMgr->lastError.empty() ? "Failed to enable" : btMgr->lastError;
        }
      }
      requestUpdate();
    } else if (selectedIndex == kRemoteIndex) {
      if (!btMgr->isEnabled()) {
        lastError = "Turn Bluetooth on first";
      } else if (SETTINGS.bleBondedDeviceAddr[0] != '\0') {
        // Forget the remembered remote: drop the connection and the bond.
        const std::string addr = SETTINGS.bleBondedDeviceAddr;
        if (btMgr->isConnected(addr)) {
          btMgr->disconnectFromDevice(addr);
        }
        SETTINGS.bleBondedDeviceAddr[0] = '\0';
        SETTINGS.bleBondedDeviceName[0] = '\0';
        SETTINGS.bleBondedDeviceAddrType = 0;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice("", "");
        lastError = "Remote forgotten";
      } else {
        // No remote yet: scan and show the picker.
        btMgr->startScan(10000);
        lastScanTime = millis();
        viewMode = ViewMode::DEVICE_LIST;
        selectedIndex = 0;
        lastError = "";
      }
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Calculate menu items: devices + "Refresh" + "Disconnect" (if connected)
  int menuItems = devices.size() + 1;  // +1 for Refresh
  if (!connectedDevices.empty()) {
    menuItems++;  // +1 for Disconnect
  }
  int maxIndex = menuItems - 1;

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    requestUpdate();
  }

  // Left/Right for back/refresh
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    // Go back to main menu
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    if (btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    // Quick rescan
    LOG_INF("BT", "Quick rescan...");
    lastError = "Scanning...";
    btMgr->startScan(10000);
    lastScanTime = millis();
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Check if "Refresh" is selected
    if (selectedIndex == static_cast<int>(devices.size())) {
      LOG_INF("BT", "Refreshing scan...");
      lastError = "Scanning...";
      btMgr->startScan(10000);
      lastScanTime = millis();
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    // Check if "Disconnect" is selected
    if (!connectedDevices.empty() && selectedIndex == static_cast<int>(devices.size()) + 1) {
      LOG_INF("BT", "Disconnecting from all devices...");
      // Make a copy of addresses to avoid iterator invalidation
      std::vector<std::string> deviceAddresses = connectedDevices;
      for (const auto& addr : deviceAddresses) {
        LOG_DBG("BT", "Disconnecting from %s", addr.c_str());
        btMgr->disconnectFromDevice(addr);
      }
      lastError = "Disconnected";
      selectedIndex = 0;
      requestUpdate();
      return;
    }

    // Otherwise, connect to selected device
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(devices.size())) {
      const auto& device = devices[selectedIndex];

      LOG_INF("BT", "Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
      lastError = "Connecting...";
      requestUpdate();

      if (btMgr->connectToDevice(device.address)) {
        strncpy(SETTINGS.bleBondedDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
        SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
        strncpy(SETTINGS.bleBondedDeviceName, device.name.c_str(), sizeof(SETTINGS.bleBondedDeviceName) - 1);
        SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
        SETTINGS.bleBondedDeviceAddrType = 0;
        SETTINGS.saveToFile();
        btMgr->setBondedDevice(device.address, device.name);

        lastError = "Bluetooth enabled";
        LOG_INF("BT", "Successfully connected to %s", device.name.c_str());
      } else {
        lastError = btMgr->lastError.empty() ? "Connection failed" : btMgr->lastError;
        LOG_ERR("BT", "Failed to connect: %s", lastError.c_str());
      }
      requestUpdate();
    }
  }
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::DEVICE_LIST) {
    renderDeviceList();
  }
}

void BluetoothSettingsActivity::renderMainMenu() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header with Bluetooth title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER));

  // Status subheader
  std::string statusLine;
  if (btMgr) {
    if (!btMgr->isEnabled()) {
      statusLine = "Bluetooth is off";
    } else if (SETTINGS.bleBondedDeviceAddr[0] != '\0' && btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
      statusLine =
          std::string("Connected: ") + (SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : "remote");
    } else if (SETTINGS.bleBondedDeviceAddr[0] != '\0') {
      statusLine = "Remote not connected - press its button";
    } else {
      statusLine = "No remote paired";
    }
  } else {
    statusLine = "Error initializing Bluetooth";
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    statusLine.c_str());

  const bool bonded = SETTINGS.bleBondedDeviceAddr[0] != '\0';
  std::vector<std::string> itemLabels;
  std::vector<std::string> itemValues;
  itemLabels.reserve(4);
  itemValues.reserve(4);

  itemLabels.push_back(tr(STR_BLUETOOTH));
  itemValues.push_back(btMgr && btMgr->isEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));

  if (bonded) {
    itemLabels.push_back(renderer.truncatedText(
        UI_10_FONT_ID, (std::string(tr(STR_BT_REMOTE)) + ": " + SETTINGS.bleBondedDeviceName).c_str(),
        pageWidth - metrics.contentSidePadding * 6));
    itemValues.push_back(tr(STR_BT_FORGET));
  } else {
    itemLabels.push_back(tr(STR_BT_NO_REMOTE_CONNECT));
    itemValues.push_back("");
  }

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      static_cast<int>(itemLabels.size()), selectedIndex, [&itemLabels](int index) { return itemLabels[index]; },
      nullptr, nullptr, [&itemValues](int i) { return i < (int)itemValues.size() ? itemValues[i] : std::string(""); },
      true);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int statusY = pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - lineH;

  // Expectation-setting: this feature is experimental and trades RAM for the
  // remote, so tell the user up-front why Bluetooth keeps turning itself off.
  {
    const auto explainerLines =
        renderer.wrappedText(UI_10_FONT_ID, tr(STR_BT_EXPLAINER), pageWidth - metrics.contentSidePadding * 2, 8);
    const int explainerY = statusY - static_cast<int>(explainerLines.size()) * lineH - metrics.verticalSpacing;
    for (int i = 0; i < static_cast<int>(explainerLines.size()); i++) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, explainerY + i * lineH, explainerLines[i].c_str(),
                        true);
    }
  }

  if (!lastError.empty()) {
    std::string statusText =
        renderer.truncatedText(UI_10_FONT_ID, lastError.c_str(), pageWidth - metrics.contentSidePadding * 2);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, statusY, statusText.c_str(), true);
  }

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDeviceList() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  if (!btMgr) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Bluetooth error");
    return;
  }

  const auto& devices = btMgr->getDiscoveredDevices();
  const auto& connectedDevices = btMgr->getConnectedDevices();

  // Header with device count
  char countStr[32];
  snprintf(countStr, sizeof(countStr), btMgr->isScanning() ? tr(STR_SCANNING) : "Found %zu", devices.size());
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER),
                 countStr);

  // Subheader with scan status
  std::string subheaderText;
  if (btMgr->isScanning()) {
    subheaderText = "Searching for devices...";
  } else {
    if (devices.empty()) {
      subheaderText = "No devices found";
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%d device(s) available", (int)devices.size());
      subheaderText = buf;
    }
  }

  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subheaderText.c_str());

  // Build device list labels. `GUI.drawList()` already paginates based on
  // `selectedIndex`, so keep the full device list here and let the user scroll
  // through every discovered device instead of truncating after the first page.
  std::vector<std::string> deviceLabels;
  std::vector<std::string> deviceValues;
  char buf[128];

  if (!devices.empty()) {
    for (const auto& device : devices) {
      const bool connected = btMgr->isConnected(device.address);

      // Device name with indicators
      const char* connSymbol = connected ? "[*] " : "";
      const char* hidSymbol = device.isHID ? "[HID] " : "";
      snprintf(buf, sizeof(buf), "%s%s%s", connSymbol, hidSymbol, device.name.c_str());
      deviceLabels.push_back(buf);

      // RSSI/signal strength
      const std::string signalBars = getSignalStrengthIndicator(device.rssi);
      snprintf(buf, sizeof(buf), "%s (%d dBm)", signalBars.c_str(), device.rssi);
      deviceValues.push_back(buf);
    }
  }

  // Add action buttons after the full device list.
  deviceLabels.push_back("< Rescan >");
  deviceValues.push_back("");

  if (!connectedDevices.empty()) {
    deviceLabels.push_back("< Disconnect All >");
    deviceValues.push_back("");
  }

  // Render the list using GUI.drawList for consistency
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      deviceLabels.size(), selectedIndex, [&deviceLabels](int index) { return deviceLabels[index]; }, nullptr, nullptr,
      [&deviceValues](int i) { return i < (int)deviceValues.size() ? deviceValues[i] : std::string(""); }, true);

  // Help text
  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   "Up/Down: Scroll | Right: Rescan");

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), tr(STR_DIR_LEFT), tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string BluetoothSettingsActivity::getSignalStrengthIndicator(const int32_t rssi) const {
  // BLE RSSI tends to be lower than WiFi at similar distance.
  // Use BLE-friendly thresholds so nearby remotes are not shown as always weak.
  if (rssi >= -60) {
    return "||||";  // Excellent
  }
  if (rssi >= -70) {
    return " |||";  // Good
  }
  if (rssi >= -80) {
    return "  ||";  // Fair
  }
  return "   |";  // Very weak
}
