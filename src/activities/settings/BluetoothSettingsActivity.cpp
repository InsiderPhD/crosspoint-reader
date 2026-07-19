#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "DeviceProfiles.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  lastScanTime = 0;
  resetLearnState();
  debugLastKeycode = 0;
  debugEventCount = 0;
  debugLastEventMs = 0;
  debugUniqueCount = 0;
  memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
  memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));

  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  if (btMgr) {
    btMgr->setLearnInputCallback(nullptr);
    btMgr->setInputCallback(nullptr);
    btMgr->setInjectionSuppressed(false);
  }
  Activity::onExit();
}

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
    } else if (viewMode == ViewMode::LEARN_KEYS) {
      if (learnCapturing) {
        // Cancel the pending capture but stay in the wizard.
        learnCapturing = false;
        pendingFrameValid = false;
        capCount = 0;
        lastError = "Capture canceled";
      } else {
        if (btMgr) {
          btMgr->setLearnInputCallback(nullptr);
          btMgr->setInjectionSuppressed(false);
        }
        viewMode = ViewMode::MAIN_MENU;
        selectedIndex = 0;
        lastError = "Wizard closed (Save to keep keys)";
      }
      requestUpdate();
      return;
    } else if (viewMode == ViewMode::DEBUG_MONITOR) {
      if (btMgr) {
        btMgr->setInputCallback(nullptr);
      }
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
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
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    handleDebugInput();
  } else {
    handleLearnInput();
  }
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  constexpr int kMainMenuItemCount =
#ifdef ENABLE_BT_DEBUG_MONITOR
      4;
#else
      3;
#endif

  constexpr int kToggleBluetoothIndex = 0;
  constexpr int kRemoteIndex = 1;
  constexpr int kMapRemoteIndex = 2;
#ifdef ENABLE_BT_DEBUG_MONITOR
  constexpr int kDebugMonitorIndex = 3;
#endif

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
        DeviceProfiles::clearCustomProfileForDevice(addr);
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
    } else if (selectedIndex == kMapRemoteIndex) {
      // Mapping needs a live connection. If we remember a remote but it's
      // disconnected, try reconnecting right here — the user likely just
      // handled it, so it should be awake and connectable.
      if (btMgr->isEnabled() && btMgr->getConnectedDevices().empty() && SETTINGS.bleBondedDeviceAddr[0] != '\0') {
        lastError = "Reconnecting...";
        requestUpdateAndWait();
        btMgr->connectToDevice(SETTINGS.bleBondedDeviceAddr);
      }
      if (!btMgr->isEnabled()) {
        lastError = "Enable BT first";
      } else if (btMgr->getConnectedDevices().empty()) {
        lastError = "Press a button on the remote, then retry";
      } else {
        viewMode = ViewMode::LEARN_KEYS;
        selectedIndex = kLearnRowForward;
        resetLearnState();
        // Keep parsing HID reports for capture/testing, but stop injecting them as
        // virtual button presses — otherwise the remote (or its connection noise)
        // drives this menu while its keys are being learned.
        btMgr->setInjectionSuppressed(true);
        // Raw-frame mailbox: NimBLE task writes one frame, loop() consumes it.
        // All-zero frames (classic key-release reports) are never useful here.
        btMgr->setLearnInputCallback([this](const uint8_t* data, size_t length) {
          if (viewMode != ViewMode::LEARN_KEYS || length == 0 || pendingFrameValid) {
            return;
          }
          const size_t n = length < kLearnFrameLen ? length : kLearnFrameLen;
          bool allZero = true;
          for (size_t i = 0; i < n; i++) {
            if (data[i] != 0x00) {
              allZero = false;
              break;
            }
          }
          if (allZero) {
            return;
          }
          memcpy(pendingFrame, data, n);
          pendingFrameLen = static_cast<uint8_t>(n);
          pendingFrameValid = true;  // written last: consumer clears it first
        });
        lastError = "";
      }
      requestUpdate();
    }
#ifdef ENABLE_BT_DEBUG_MONITOR
    else if (selectedIndex == kDebugMonitorIndex) {
      if (!btMgr->isDebugCaptureEnabled()) {
        btMgr->setDebugCaptureEnabled(true);
      }
      debugLastKeycode = 0;
      debugEventCount = 0;
      debugLastEventMs = 0;
      debugUniqueCount = 0;
      memset(debugUniqueKeys, 0, sizeof(debugUniqueKeys));
      memset(debugUniqueCounts, 0, sizeof(debugUniqueCounts));
      btMgr->setInputCallback([this](uint16_t keycode) {
        debugLastKeycode = keycode & 0xFF;
        debugEventCount++;
        debugLastEventMs = millis();

        const uint8_t code = static_cast<uint8_t>(keycode & 0xFF);
        bool found = false;
        for (uint8_t i = 0; i < debugUniqueCount; i++) {
          if (debugUniqueKeys[i] == code) {
            if (debugUniqueCounts[i] < 65535) {
              debugUniqueCounts[i]++;
            }
            found = true;
            break;
          }
        }

        if (!found && debugUniqueCount < kDebugUniqueKeyMax) {
          debugUniqueKeys[debugUniqueCount] = code;
          debugUniqueCounts[debugUniqueCount] = 1;
          debugUniqueCount++;
        }
      });
      viewMode = ViewMode::DEBUG_MONITOR;
      lastError = "BT debug monitor";
      requestUpdate();
    }
#endif
  }
}

void BluetoothSettingsActivity::resetLearnState() {
  learnCapturing = false;
  learnOneButton = false;
  pendingFrameValid = false;
  pendingFrameLen = 0;
  capLen = 0;
  capCount = 0;
  capFirstMs = 0;
  fwdLen = 0;
  fwdCaptured = false;
  backLen = 0;
  backCaptured = false;
  learnedPrevKey = 0;
  learnedPrevIdx = 0xFF;
  learnedNextKey = 0;
  learnedNextIdx = 0xFF;
  learnLastTestDir = 0xFF;
  learnLastTestMs = 0;
}

// Close out an armed capture: store the accumulated frame + stability mask into
// the slot for the armed direction, then recompute the mapping if both are set.
void BluetoothSettingsActivity::finalizeLearnCapture() {
  learnCapturing = false;
  if (capCount == 0) {
    lastError = "Nothing received - try again";
    return;
  }

  if (selectedIndex == kLearnRowForward) {
    memcpy(fwdFrame, capFrame, sizeof(fwdFrame));
    memcpy(fwdStable, capStable, sizeof(fwdStable));
    fwdLen = capLen;
    fwdCaptured = true;
  } else {
    memcpy(backFrame, capFrame, sizeof(backFrame));
    memcpy(backStable, capStable, sizeof(backStable));
    backLen = capLen;
    backCaptured = true;
  }
  capCount = 0;
  lastError = "";

  if (!computeLearnMapping()) {
    lastError = (fwdCaptured && backCaptured) ? "Buttons look identical - recapture one"
                                              : "Couldn't read that button - try again";
  }
}

// Derive a byte position from a single captured frame (one-button mapping):
// among the bytes that stayed stable across the press, prefer a known HID
// page-turn code, then the classic keyboard key slots, then anything non-zero.
static bool deriveSingleFrameKey(const uint8_t* frame, const bool* stable, const uint8_t len, uint8_t& outKey,
                                 uint8_t& outIdx) {
  for (uint8_t i = 0; i < len; i++) {
    if (stable[i] && frame[i] != 0x00 && DeviceProfiles::isCommonPageTurnCode(frame[i])) {
      outKey = frame[i];
      outIdx = i;
      return true;
    }
  }
  for (uint8_t i = 2; i < len; i++) {
    if (stable[i] && frame[i] != 0x00) {
      outKey = frame[i];
      outIdx = i;
      return true;
    }
  }
  for (uint8_t i = 0; i < len; i++) {
    if (stable[i] && frame[i] != 0x00) {
      outKey = frame[i];
      outIdx = i;
      return true;
    }
  }
  return false;
}

// Derive the mapping from the captured frames. With both directions captured,
// diff them to find the byte position(s) that distinguish the buttons (bytes
// that varied within a single press — rolling counters, joystick axes — are
// excluded via the stability masks). With only one direction captured, fall
// back to the single-frame heuristic so one-button remotes can be mapped.
bool BluetoothSettingsActivity::computeLearnMapping() {
  learnedPrevKey = 0;
  learnedPrevIdx = 0xFF;
  learnedNextKey = 0;
  learnedNextIdx = 0xFF;

  if (fwdCaptured != backCaptured) {
    // Single-direction mapping.
    if (fwdCaptured) {
      return deriveSingleFrameKey(fwdFrame, fwdStable, fwdLen, learnedNextKey, learnedNextIdx);
    }
    return deriveSingleFrameKey(backFrame, backStable, backLen, learnedPrevKey, learnedPrevIdx);
  }
  if (!fwdCaptured && !backCaptured) {
    return false;
  }

  const uint8_t len = fwdLen < backLen ? fwdLen : backLen;

  // Preferred: one position where both directions hold distinct non-zero values
  // (e.g. keyboard code slot, or the Game-Brick-style 0x07/0x09 button byte).
  for (uint8_t i = 0; i < len; i++) {
    if (fwdStable[i] && backStable[i] && fwdFrame[i] != backFrame[i] && fwdFrame[i] != 0x00 && backFrame[i] != 0x00) {
      learnedNextKey = fwdFrame[i];
      learnedNextIdx = i;
      learnedPrevKey = backFrame[i];
      learnedPrevIdx = i;
      return true;
    }
  }

  // Fallback: each direction sets its own byte (zero in the other direction).
  for (uint8_t i = 0; i < len; i++) {
    if (!fwdStable[i] || !backStable[i] || fwdFrame[i] == backFrame[i]) {
      continue;
    }
    if (fwdFrame[i] != 0x00 && learnedNextIdx == 0xFF) {
      learnedNextKey = fwdFrame[i];
      learnedNextIdx = i;
    }
    if (backFrame[i] != 0x00 && learnedPrevIdx == 0xFF) {
      learnedPrevKey = backFrame[i];
      learnedPrevIdx = i;
    }
  }

  if (learnedNextIdx != 0xFF && learnedPrevIdx != 0xFF) {
    return true;
  }

  learnedPrevKey = 0;
  learnedPrevIdx = 0xFF;
  learnedNextKey = 0;
  learnedNextIdx = 0xFF;
  return false;
}

void BluetoothSettingsActivity::handleLearnInput() {
  // Consume a raw frame handed over by the learn callback.
  if (pendingFrameValid) {
    uint8_t frame[kLearnFrameLen];
    const uint8_t frameLen = pendingFrameLen;
    memcpy(frame, pendingFrame, sizeof(frame));
    pendingFrameValid = false;

    if (learnCapturing) {
      if (capCount == 0) {
        memcpy(capFrame, frame, sizeof(capFrame));
        capLen = frameLen;
        for (size_t i = 0; i < kLearnFrameLen; i++) {
          capStable[i] = true;
        }
        capFirstMs = millis();
        capCount = 1;
        requestUpdate();  // show that something arrived
      } else {
        const uint8_t n = frameLen < capLen ? frameLen : capLen;
        for (uint8_t i = 0; i < n; i++) {
          if (frame[i] != capFrame[i]) {
            capStable[i] = false;
          }
        }
        capCount++;
        // A few frames are enough to identify the counter/axis bytes, and
        // finishing quickly keeps release-tail frames out of the sample.
        if (capCount >= 3) {
          finalizeLearnCapture();
          requestUpdate();
        }
      }
      return;
    }

    // Not capturing: once any mapping is computed, act as a live tester.
    if (learnedNextIdx != 0xFF || learnedPrevIdx != 0xFF) {
      const bool fwdMatch =
          learnedNextIdx != 0xFF && frameLen > learnedNextIdx && frame[learnedNextIdx] == learnedNextKey;
      const bool backMatch =
          learnedPrevIdx != 0xFF && frameLen > learnedPrevIdx && frame[learnedPrevIdx] == learnedPrevKey;
      if (learnOneButton && (fwdMatch || backMatch)) {
        learnLastTestDir = 0x01;
        learnLastTestMs = millis();
        requestUpdate();
      } else if (fwdMatch && !backMatch) {
        learnLastTestDir = 0x01;
        learnLastTestMs = millis();
        requestUpdate();
      } else if (backMatch && !fwdMatch) {
        learnLastTestDir = 0x00;
        learnLastTestMs = millis();
        requestUpdate();
      }
    }
    return;
  }

  // Single-frame presses: if no further frames arrive, close the capture on a
  // short timeout instead of waiting forever for a 3-frame sample.
  if (learnCapturing && capCount > 0 && (millis() - capFirstMs) > 500) {
    finalizeLearnCapture();
    requestUpdate();
    return;
  }

  // Clear a stale tester flash after a short hold so the label doesn't stick.
  if (learnLastTestDir != 0xFF && (millis() - learnLastTestMs) > 2000) {
    learnLastTestDir = 0xFF;
    requestUpdate();
  }

  // While armed, only the remote (or Back = cancel, handled in loop()) matters.
  if (learnCapturing) {
    return;
  }

  // Menu navigation with the device buttons, matching the other list screens.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (kLearnRowCount - 1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex < (kLearnRowCount - 1)) ? selectedIndex + 1 : 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == kLearnRowForward || selectedIndex == kLearnRowBack) {
      learnCapturing = true;
      pendingFrameValid = false;
      capCount = 0;
      lastError = "";
      requestUpdate();
      return;
    }

    if (selectedIndex == kLearnRowOneButton) {
      learnOneButton = !learnOneButton;
      requestUpdate();
      return;
    }

    // Save & finish (one mapped direction is enough — single-button remotes).
    if (learnedNextIdx == 0xFF && learnedPrevIdx == 0xFF) {
      lastError =
          (fwdCaptured && backCaptured) ? "Buttons look identical - recapture one" : "Set at least one key first";
      requestUpdate();
      return;
    }
    const uint8_t baseIdx = (learnedPrevIdx != 0xFF) ? learnedPrevIdx : learnedNextIdx;
    DeviceProfiles::setCustomProfile(learnedPrevKey, learnedNextKey, baseIdx, learnedPrevIdx, learnedNextIdx,
                                     learnOneButton);
    if (btMgr) {
      const auto& connected = btMgr->getConnectedDevices();
      for (const auto& addr : connected) {
        DeviceProfiles::setCustomProfileForDevice(addr, learnedPrevKey, learnedNextKey, baseIdx, learnedPrevIdx,
                                                  learnedNextIdx, learnOneButton);
      }
      btMgr->setLearnInputCallback(nullptr);
      btMgr->setInjectionSuppressed(false);
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "Saved! Back=0x%02X@%u Fwd=0x%02X@%u", learnedPrevKey,
             static_cast<unsigned>(learnedPrevIdx), learnedNextKey, static_cast<unsigned>(learnedNextIdx));
    lastError = buf;
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
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
  } else if (viewMode == ViewMode::DEBUG_MONITOR) {
    renderDebugMonitor();
  } else {
    renderLearnKeys();
  }
}

void BluetoothSettingsActivity::handleDebugInput() {
  if (!btMgr) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const bool next = !btMgr->isDebugCaptureEnabled();
    btMgr->setDebugCaptureEnabled(next);
    lastError = next ? "BT debug capture: ON" : "BT debug capture: OFF";
    requestUpdate();
    return;
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

  itemLabels.push_back(tr(STR_BT_MAP_REMOTE));
  itemValues.push_back("");

#ifdef ENABLE_BT_DEBUG_MONITOR
  itemLabels.push_back("BT Debug Monitor");
  itemValues.push_back(btMgr && btMgr->isDebugCaptureEnabled() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
#endif

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

void BluetoothSettingsActivity::renderLearnKeys() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Remote Setup Wizard");

  const char* subText =
      learnCapturing ? "Press a button on your REMOTE now" : "Select a row, Confirm, then press the remote";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subText);

  // Menu rows, rendered like the other settings lists.
  char forwardValue[32];
  char backValue[32];
  if (learnCapturing && selectedIndex == kLearnRowForward) {
    snprintf(forwardValue, sizeof(forwardValue), "Press remote...");
  } else if (learnedNextIdx != 0xFF) {
    snprintf(forwardValue, sizeof(forwardValue), "0x%02X @%u", learnedNextKey, static_cast<unsigned>(learnedNextIdx));
  } else if (fwdCaptured) {
    snprintf(forwardValue, sizeof(forwardValue), "Captured");
  } else {
    snprintf(forwardValue, sizeof(forwardValue), "Not set");
  }
  if (learnCapturing && selectedIndex == kLearnRowBack) {
    snprintf(backValue, sizeof(backValue), "Press remote...");
  } else if (learnedPrevIdx != 0xFF) {
    snprintf(backValue, sizeof(backValue), "0x%02X @%u", learnedPrevKey, static_cast<unsigned>(learnedPrevIdx));
  } else if (backCaptured) {
    snprintf(backValue, sizeof(backValue), "Captured");
  } else {
    snprintf(backValue, sizeof(backValue), "Not set");
  }

  const char* rowLabels[kLearnRowCount] = {learnOneButton ? "Button code 1" : "Forward key",
                                           learnOneButton ? "Button code 2" : "Back key", "One-button remote",
                                           "< Save & Finish >"};
  const std::string rowValues[kLearnRowCount] = {forwardValue, backValue,
                                                 learnOneButton ? tr(STR_STATE_ON) : tr(STR_STATE_OFF), ""};

  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2 + 60)},
      kLearnRowCount, selectedIndex, [&rowLabels](int index) { return std::string(rowLabels[index]); }, nullptr,
      nullptr, [&rowValues](int i) { return i < kLearnRowCount ? rowValues[i] : std::string(""); }, true);

  // Live tester: once both keys are set, flash which one the remote just sent.
  const int testerY = pageHeight - metrics.buttonHintsHeight - 40;
  if (learnedNextIdx != 0xFF || learnedPrevIdx != 0xFF) {
    if (learnLastTestDir == 0x01) {
      renderer.drawCenteredText(UI_12_FONT_ID, testerY, ">> FORWARD pressed <<");
    } else if (learnLastTestDir == 0x00) {
      renderer.drawCenteredText(UI_12_FONT_ID, testerY, "<< BACK pressed >>");
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, testerY, "Test: press the remote buttons");
    }
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), learnCapturing ? "" : tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void BluetoothSettingsActivity::renderDebugMonitor() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Bluetooth Debug");

  std::string sub = btMgr && btMgr->isDebugCaptureEnabled() ? "Capture ON" : "Capture OFF";
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    sub.c_str());

  char line1[64];
  char line2[64];
  char line3[64];
  char line4[64];

  unsigned int connectedCount = btMgr ? static_cast<unsigned int>(btMgr->getConnectedDevices().size()) : 0;
  snprintf(line1, sizeof(line1), "Connected: %u", connectedCount);
  snprintf(line2, sizeof(line2), "Key events: %u", static_cast<unsigned>(debugEventCount));
  snprintf(line3, sizeof(line3), "Unique keys: %u", static_cast<unsigned>(debugUniqueCount));
  snprintf(line4, sizeof(line4), "Last key: 0x%02X", static_cast<unsigned>(debugLastKeycode & 0xFF));

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 24,
                            line1);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 48,
                            line2);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 72,
                            line3);
  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 96,
                            line4);

  if (debugLastEventMs > 0) {
    char eventAgeLine[64];
    snprintf(eventAgeLine, sizeof(eventAgeLine), "Last event: %lus ago", (millis() - debugLastEventMs) / 1000);
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 114,
                              eventAgeLine);
  }

  const int uniqueStartY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + 132;
  if (debugUniqueCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY, "No key presses captured yet");
  } else {
    uint8_t sortedIndices[kDebugUniqueKeyMax] = {0};
    for (uint8_t i = 0; i < debugUniqueCount; i++) {
      sortedIndices[i] = i;
    }

    for (uint8_t i = 0; i + 1 < debugUniqueCount; i++) {
      uint8_t best = i;
      for (uint8_t j = i + 1; j < debugUniqueCount; j++) {
        const uint16_t bestCount = debugUniqueCounts[sortedIndices[best]];
        const uint16_t candidateCount = debugUniqueCounts[sortedIndices[j]];
        if (candidateCount > bestCount) {
          best = j;
        }
      }
      if (best != i) {
        const uint8_t tmp = sortedIndices[i];
        sortedIndices[i] = sortedIndices[best];
        sortedIndices[best] = tmp;
      }
    }

    const uint8_t renderCount = (debugUniqueCount < 4) ? debugUniqueCount : 4;
    for (uint8_t i = 0; i < renderCount; i++) {
      const uint8_t idx = sortedIndices[i];
      char keyLine[64];
      snprintf(keyLine, sizeof(keyLine), "Key 0x%02X  x%u", static_cast<unsigned>(debugUniqueKeys[idx]),
               static_cast<unsigned>(debugUniqueCounts[idx]));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(i) * 16, keyLine);
    }

    if (debugUniqueCount > renderCount) {
      char moreLine[48];
      snprintf(moreLine, sizeof(moreLine), "+%u more keys", static_cast<unsigned>(debugUniqueCount - renderCount));
      renderer.drawCenteredText(UI_10_FONT_ID, uniqueStartY + static_cast<int>(renderCount) * 16, moreLine);
    }
  }

  if (!lastError.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 16, lastError.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}