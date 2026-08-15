#include "BluetoothSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
// Main-menu row indices, shared by handleMainMenuInput() and
// activateMainMenuItem(). Must stay aligned with the list built in
// renderMainMenu().
constexpr int kToggleBluetoothIndex = 0;
constexpr int kRemoteIndex = 1;
constexpr int kReconnectIndex = 2;
constexpr int kMapButtonsIndex = 3;

// The manager reports outcomes as codes because it sits below I18n; translate
// them here, at the layer that actually draws them.
const char* btStatusText(const BtStatus status) {
  switch (status) {
    case BtStatus::Connected:
      return tr(STR_CONNECTED);
    case BtStatus::NotEnoughMemory:
      return tr(STR_BT_ERR_NO_MEMORY);
    case BtStatus::HeapFragmented:
      return tr(STR_BT_ERR_FRAGMENTED);
    case BtStatus::StartFailed:
      return tr(STR_BT_ERR_START_FAILED);
    case BtStatus::ScanFailed:
      return tr(STR_BT_ERR_SCAN_FAILED);
    case BtStatus::NotEnabled:
      return tr(STR_BT_ERR_NOT_ENABLED);
    case BtStatus::ClientFailed:
      return tr(STR_BT_ERR_CLIENT);
    case BtStatus::ConnectFailed:
      return tr(STR_BT_ERR_CONNECT);
    case BtStatus::NoHidService:
      return tr(STR_BT_ERR_NO_HID);
    case BtStatus::NoReportChar:
      return tr(STR_BT_ERR_NO_REPORT);
    case BtStatus::SubscribeFailed:
      return tr(STR_BT_ERR_SUBSCRIBE);
    case BtStatus::None:
      break;
  }
  return "";
}
}  // namespace

void BluetoothSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  viewMode = ViewMode::MAIN_MENU;
  lastError = "";
  selectedDeviceName.clear();
  connectionError.clear();
  // Get BLE manager instance
  btMgr = &BluetoothHIDManager::getInstance();
  LOG_INF("BT", "BluetoothHIDManager ready");

  // Diagnostic: this is the one screen where the user actively tests a remote
  // (the live press-count box below), so log every raw HID frame as [BTDBG]
  // while we're here. Lets a press that isn't turning a page be told apart from
  // a press that never arrives. Scoped to the screen (cleared in onExit) so it
  // never spams the log during normal reading.
  btMgr->setDebugCaptureEnabled(true);

  requestUpdate();
}

void BluetoothSettingsActivity::onExit() {
  // Never leave the radio scanning behind us — it costs power and keeps the
  // NimBLE callbacks firing into an activity that is about to be deleted.
  if (btMgr && btMgr->isScanning()) {
    btMgr->stopScan();
  }
  // Stop the raw-frame [BTDBG] logging enabled in onEnter — it is a debugging
  // aid for this screen only, not something to leave running behind us.
  if (btMgr) {
    btMgr->setDebugCaptureEnabled(false);
    // Belt-and-braces: the mapping wizard turns this on, and leaving it set
    // would silently disable the remote everywhere.
    btMgr->setInjectionSuppressed(false);
  }
  Activity::onExit();
}

void BluetoothSettingsActivity::loop() {
  // Live remote test box: a new report repaints the main menu so the user gets
  // immediate confirmation their remote is talking to the device.
  if (viewMode == ViewMode::MAIN_MENU && pollRemoteTest()) {
    requestUpdate();
    return;
  }

  // The scan runs on its own timer inside the manager; when it stops, fall
  // through to the results the same way WifiSelectionActivity does.
  if (viewMode == ViewMode::SCANNING && btMgr && !btMgr->isScanning()) {
    viewMode = ViewMode::DEVICE_LIST;
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  switch (viewMode) {
    case ViewMode::MAIN_MENU:
      handleMainMenuInput();
      break;
    case ViewMode::SCANNING:
      handleScanningInput();
      break;
    case ViewMode::DEVICE_LIST:
      handleDeviceListInput();
      break;
    case ViewMode::CONNECTED:
    case ViewMode::CONNECT_FAILED:
      handleResultInput();
      break;
    case ViewMode::MAP_BACK:
    case ViewMode::MAP_FORWARD:
      handleMappingInput();
      pollMappingPress();
      break;
    case ViewMode::CONNECTING:
      // Transient: connectToDevice() blocks, so this state is only ever on
      // screen while that call runs and there is no input to service.
      break;
  }
}

void BluetoothSettingsActivity::beginScan() {
  if (!btMgr) return;
  LOG_INF("BT", "Scanning for devices...");
  connectionError.clear();
  lastError = "";
  selectedIndex = 0;
  viewMode = ViewMode::SCANNING;
  btMgr->startScan(SCAN_DURATION_MS);
  requestUpdate();
}

void BluetoothSettingsActivity::handleMainMenuInput() {
  // The reconnect and mapping rows only exist once there is a remote, mirroring
  // the list built in renderMainMenu().
  const bool bonded = SETTINGS.bleBondedDeviceAddr[0] != '\0';
  const int kMainMenuItemCount = bonded ? 4 : 2;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), kMainMenuItemCount, selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      activateMainMenuItem();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  // A remote press is injected as a virtual page-forward, which arrives here as
  // a side-button press and would scroll the menu while the user is only trying
  // to test the remote. The test box is the feedback, so swallow the movement.
  const bool fromRemote = btMgr && btMgr->hadRecentRemoteInput(400);

  if (!fromRemote) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : (kMainMenuItemCount - 1);
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      selectedIndex = (selectedIndex < (kMainMenuItemCount - 1)) ? selectedIndex + 1 : 0;
      requestUpdate();
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateMainMenuItem();
  }
}

void BluetoothSettingsActivity::activateMainMenuItem() {
  if (!btMgr) {
    lastError = tr(STR_BT_NOT_AVAILABLE);
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
      // Clear the session flag so auto-restore doesn't bring the stack back
      // after the user explicitly turned it off here.
      btMgr->setBluetoothWanted(false);
      if (btMgr->disable()) {
        lastError = tr(STR_BT_DISABLED);
      } else {
        lastError = tr(STR_BT_DISABLE_FAILED);
      }
    } else {
      LOG_INF("BT", "Enabling Bluetooth...");
      // Arm auto-restore: the remote should return by itself after later
      // memory-critical operations (chapter builds, syncs) tear the stack down.
      btMgr->setBluetoothWanted(true);
      if (btMgr->enable()) {
        lastError = tr(STR_BT_ENABLED);
      } else {
        lastError = btStatusText(btMgr->lastStatus);
      }
    }
    requestUpdate();
  } else if (selectedIndex == kRemoteIndex) {
    if (SETTINGS.bleBondedDeviceAddr[0] != '\0') {
      if (!btMgr->isEnabled()) {
        lastError = tr(STR_BT_TURN_ON_FIRST);
        requestUpdate();
        return;
      }
      // Forget the remembered remote: drop the connection and the bond.
      const std::string addr = SETTINGS.bleBondedDeviceAddr;
      if (btMgr->isConnected(addr)) {
        btMgr->disconnectFromDevice(addr);
      }
      SETTINGS.bleBondedDeviceAddr[0] = '\0';
      SETTINGS.bleBondedDeviceName[0] = '\0';
      SETTINGS.bleBondedDeviceAddrType = 0;
      // The learned button mapping belongs to the forgotten remote.
      SETTINGS.bleBackSigIndex = 0xFF;
      SETTINGS.bleBackSigValue = 0;
      SETTINGS.bleFwdSigIndex = 0xFF;
      SETTINGS.bleFwdSigValue = 0;
      SETTINGS.saveToFile();
      btMgr->setBondedDevice("", "");
      btMgr->setButtonMapping(0xFF, 0, 0xFF, 0);
      lastError = tr(STR_BT_REMOTE_FORGOTTEN);
      requestUpdate();
    } else {
      // First-time guided setup: enable -> scan -> pair -> map -> test. Turn
      // the radio on ourselves instead of bouncing the user back with "turn
      // Bluetooth on first" — the row's whole purpose is getting a remote
      // working.
      if (!btMgr->isEnabled()) {
        {
          RenderLock lock(*this);
          GUI.drawPopup(renderer, tr(STR_BT_TURNING_ON));
          renderer.displayBuffer();
        }
        // Arm auto-restore, same as the toggle row: the stack should return by
        // itself after memory-critical operations tear it down.
        btMgr->setBluetoothWanted(true);
        if (!btMgr->enable()) {
          // Surface the specific reason (fragmented heap, no memory, ...) the
          // same way the toggle row does.
          lastError = btStatusText(btMgr->lastStatus);
          requestUpdate();
          return;
        }
      }
      guidedSetup = true;
      beginScan();
    }
  } else if (selectedIndex == kReconnectIndex) {
    reconnectBonded();
  } else if (selectedIndex == kMapButtonsIndex) {
    beginButtonMapping();
  }
}

void BluetoothSettingsActivity::handleScanningInput() {
  // Backing out of a scan is the one input this screen takes — matching the
  // WiFi flow, where Back during a scan returns you to where you came from.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (btMgr && btMgr->isScanning()) {
      btMgr->stopScan();
    }
    guidedSetup = false;
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
  }
}

void BluetoothSettingsActivity::handleDeviceListInput() {
  if (!btMgr) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    guidedSetup = false;
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  // Rescan sits on a button rather than in the list, so the list only ever
  // contains real devices (WifiSelectionActivity puts Retry in the same place).
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    beginScan();
    return;
  }

  const auto& devices = btMgr->getDiscoveredDevices();

  // Left mirrors WiFi's conditional third button (Forget there, Disconnect
  // here) — only meaningful while something is actually connected.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (!btMgr->getConnectedDevices().empty()) {
      disconnectAll();
    }
    return;
  }

  if (devices.empty()) {
    // Empty list: Confirm rescans, matching "Press OK to scan again". A tap
    // arrives here as an injected Confirm (handlesDirectTouch is false for the
    // empty list), so it rescans too.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      beginScan();
    }
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), static_cast<int>(devices.size()), selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      connectToSelected();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  const int maxIndex = static_cast<int>(devices.size()) - 1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selectedIndex = (selectedIndex > 0) ? selectedIndex - 1 : maxIndex;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selectedIndex = (selectedIndex < maxIndex) ? selectedIndex + 1 : 0;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    connectToSelected();
  }
}

void BluetoothSettingsActivity::handleResultInput() {
  const bool confirm = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  const bool back = mappedInput.wasPressed(MappedInputManager::Button::Back);
  if (!confirm && !back) {
    return;
  }

  if (viewMode == ViewMode::CONNECTED) {
    connectingBonded = false;
    if (guidedSetup && confirm) {
      // Guided setup: the freshly paired remote is connected, which is exactly
      // the wizard's precondition — flow straight into teaching its buttons.
      // Land on the menu first so the wizard's own error paths (remote dropped
      // in the meantime) have somewhere sensible to show their message.
      viewMode = ViewMode::MAIN_MENU;
      selectedIndex = 0;
      beginButtonMapping();
      return;
    }
    // Done: the remote is paired and remembered, so there is nothing left to do
    // on the picker — return to the screen that shows its status.
    guidedSetup = false;
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
  } else if (connectingBonded) {
    // Failed bonded reconnect: there is no scanned device list to fall back
    // to, so Retry re-attempts the same remote and Back returns to the menu.
    if (confirm) {
      reconnectBonded();
      return;
    }
    connectingBonded = false;
    viewMode = ViewMode::MAIN_MENU;
    selectedIndex = 0;
  } else {
    // Failed: Retry goes back to the list so another device can be picked.
    viewMode = ViewMode::DEVICE_LIST;
  }
  requestUpdate();
}

void BluetoothSettingsActivity::connectToSelected() {
  // This is the scan flow: a failed result must fall back to the device list,
  // not inherit the bonded-reconnect retry behaviour.
  connectingBonded = false;
  const auto& devices = btMgr->getDiscoveredDevices();
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(devices.size())) {
    return;
  }
  const auto device = devices[selectedIndex];  // copy: connecting mutates the scan results
  selectedDeviceName = device.name;

  LOG_INF("BT", "Connecting to %s (%s)", device.name.c_str(), device.address.c_str());
  // connectToDevice() blocks for several seconds, so paint the connecting
  // screen and wait for it to land before starting.
  viewMode = ViewMode::CONNECTING;
  requestUpdateAndWait();

  if (btMgr->connectToDevice(device.address)) {
    strncpy(SETTINGS.bleBondedDeviceAddr, device.address.c_str(), sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
    SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
    strncpy(SETTINGS.bleBondedDeviceName, device.name.c_str(), sizeof(SETTINGS.bleBondedDeviceName) - 1);
    SETTINGS.bleBondedDeviceName[sizeof(SETTINGS.bleBondedDeviceName) - 1] = '\0';
    // Persist the address TYPE from the advertisement: random-address remotes
    // (e.g. ff:..) ignore reconnects that target them as BLE_ADDR_PUBLIC.
    SETTINGS.bleBondedDeviceAddrType = device.addrType;
    SETTINGS.saveToFile();
    btMgr->setBondedDevice(device.address, device.name, device.addrType);

    viewMode = ViewMode::CONNECTED;
    lastError = tr(STR_BT_ENABLED);
    LOG_INF("BT", "Successfully connected to %s", device.name.c_str());
  } else {
    connectionError = btStatusText(btMgr->lastStatus);
    viewMode = ViewMode::CONNECT_FAILED;
    LOG_ERR("BT", "Failed to connect: %s", connectionError.c_str());
  }
  requestUpdate();
}

void BluetoothSettingsActivity::reconnectBonded() {
  // The early returns land on the main menu (where lastError renders): this can
  // be invoked from the failed-result screen's Retry, not just from the menu.
  if (!btMgr->isEnabled()) {
    lastError = tr(STR_BT_TURN_ON_FIRST);
    connectingBonded = false;
    viewMode = ViewMode::MAIN_MENU;
    requestUpdate();
    return;
  }
  const std::string addr = SETTINGS.bleBondedDeviceAddr;
  if (btMgr->isConnected(addr)) {
    // Already connected (the background scan may have beaten us to it) — no
    // point flashing the connecting screen.
    lastError = tr(STR_CONNECTED);
    connectingBonded = false;
    viewMode = ViewMode::MAIN_MENU;
    requestUpdate();
    return;
  }

  connectingBonded = true;
  connectionError.clear();
  lastError = "";
  selectedDeviceName = SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : tr(STR_BT_A_REMOTE);

  LOG_INF("BT", "Reconnecting to bonded remote %s", addr.c_str());
  // connectToDevice() blocks for several seconds, so paint the connecting
  // screen and wait for it to land before starting.
  viewMode = ViewMode::CONNECTING;
  requestUpdateAndWait();

  // The bond (address, name, type) is already persisted — nothing to save here.
  if (btMgr->connectToDevice(addr)) {
    viewMode = ViewMode::CONNECTED;
    LOG_INF("BT", "Reconnected to bonded remote %s", addr.c_str());
  } else {
    connectionError = btStatusText(btMgr->lastStatus);
    viewMode = ViewMode::CONNECT_FAILED;
    LOG_ERR("BT", "Bonded reconnect failed: %s", connectionError.c_str());
  }
  requestUpdate();
}

void BluetoothSettingsActivity::disconnectAll() {
  LOG_INF("BT", "Disconnecting from all devices...");
  // Copy the addresses first: disconnecting mutates the manager's list.
  const std::vector<std::string> deviceAddresses = btMgr->getConnectedDevices();
  for (const auto& addr : deviceAddresses) {
    LOG_DBG("BT", "Disconnecting from %s", addr.c_str());
    btMgr->disconnectFromDevice(addr);
  }
  lastError = tr(STR_BT_DISCONNECTED);
  requestUpdate();
}

void BluetoothSettingsActivity::beginButtonMapping() {
  if (!btMgr->isEnabled()) {
    guidedSetup = false;  // guided chain ends here; message shows on the menu
    lastError = tr(STR_BT_TURN_ON_FIRST);
    requestUpdate();
    return;
  }
  if (!btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
    guidedSetup = false;
    lastError = tr(STR_BT_MAP_NEED_CONNECTED);
    requestUpdate();
    return;
  }
  // Captured presses must not double as navigation while the wizard runs.
  btMgr->setInjectionSuppressed(true);
  lastSeenMapPressMs = btMgr->lastRemoteInputMs();
  mapStepPresses = 0;
  lastError = "";
  viewMode = ViewMode::MAP_BACK;
  requestUpdate();
}

void BluetoothSettingsActivity::handleMappingInput() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    // In the guided flow Back means "skip this step", not "undo": clear any
    // mapping (it would belong to a previous remote) and finish the setup.
    endButtonMapping(guidedSetup ? MappingOutcome::SKIPPED : MappingOutcome::CANCELLED);
  }
}

void BluetoothSettingsActivity::pollMappingPress() {
  const unsigned long stamp = btMgr->lastRemoteInputMs();
  if (stamp == 0 || stamp == lastSeenMapPressMs) {
    return;
  }
  lastSeenMapPressMs = stamp;

  uint8_t sigIndex = 0xFF;
  uint8_t sigValue = 0;
  btMgr->lastPressSignature(sigIndex, sigValue);
  if (sigIndex == 0xFF) {
    return;  // A report arrived but no signature was captured yet — ignore.
  }

  if (mapStepPresses == 0) {
    // First press of this step: remember it and ask for the confirming press.
    mapStepSigIndex = sigIndex;
    mapStepSigValue = sigValue;
    mapStepPresses = 1;
    requestUpdate();
    return;
  }

  // Second press of the step. A mismatch means the button's code changes per
  // press (a one-button toggle remote) — its presses can never be told apart,
  // so drop any mapping and fall back to everything-pages-forward.
  if (sigIndex != mapStepSigIndex || sigValue != mapStepSigValue) {
    endButtonMapping(MappingOutcome::CLEARED);
    return;
  }

  if (viewMode == ViewMode::MAP_BACK) {
    mapBackSigIndex = mapStepSigIndex;
    mapBackSigValue = mapStepSigValue;
    mapStepPresses = 0;
    viewMode = ViewMode::MAP_FORWARD;
    requestUpdate();
    return;
  }

  // Forward step confirmed. If it matches the back button, the remote (or the
  // user) isn't distinguishing two buttons — same fallback as the toggle case.
  if (mapStepSigIndex == mapBackSigIndex && mapStepSigValue == mapBackSigValue) {
    endButtonMapping(MappingOutcome::CLEARED);
    return;
  }

  SETTINGS.bleBackSigIndex = mapBackSigIndex;
  SETTINGS.bleBackSigValue = mapBackSigValue;
  SETTINGS.bleFwdSigIndex = mapStepSigIndex;
  SETTINGS.bleFwdSigValue = mapStepSigValue;
  endButtonMapping(MappingOutcome::SAVED);
}

void BluetoothSettingsActivity::endButtonMapping(const MappingOutcome outcome) {
  if (outcome == MappingOutcome::CLEARED || outcome == MappingOutcome::SKIPPED) {
    SETTINGS.bleBackSigIndex = 0xFF;
    SETTINGS.bleBackSigValue = 0;
    SETTINGS.bleFwdSigIndex = 0xFF;
    SETTINGS.bleFwdSigValue = 0;
  }
  if (outcome != MappingOutcome::CANCELLED) {
    SETTINGS.saveToFile();
    btMgr->setButtonMapping(SETTINGS.bleBackSigIndex, SETTINGS.bleBackSigValue, SETTINGS.bleFwdSigIndex,
                            SETTINGS.bleFwdSigValue);
  }
  btMgr->setInjectionSuppressed(false);
  // Keep the main menu's test box from counting the wizard's presses as one of
  // its own the moment we return.
  lastSeenRemoteInputMs = btMgr->lastRemoteInputMs();
  switch (outcome) {
    case MappingOutcome::SAVED:
      // In the guided flow, close the loop: the test box below the menu is the
      // final step, so point the user at it.
      lastError = guidedSetup ? tr(STR_BT_SETUP_DONE) : tr(STR_BT_MAP_SAVED);
      break;
    case MappingOutcome::CLEARED:
      lastError = tr(STR_BT_MAP_CANT_TELL_APART);
      break;
    case MappingOutcome::SKIPPED:
      lastError = tr(STR_BT_SETUP_DONE);
      break;
    case MappingOutcome::CANCELLED:
      lastError = "";
      break;
  }
  guidedSetup = false;
  viewMode = ViewMode::MAIN_MENU;
  requestUpdate();
}

bool BluetoothSettingsActivity::pollRemoteTest() {
  if (!btMgr || !btMgr->isEnabled()) {
    return false;
  }
  const unsigned long stamp = btMgr->lastRemoteInputMs();
  if (stamp == 0 || stamp == lastSeenRemoteInputMs) {
    return false;
  }
  lastSeenRemoteInputMs = stamp;
  lastRemotePressAtMs = millis();
  remotePressCount++;
  LOG_DBG("BT", "Remote test: press %u", static_cast<unsigned>(remotePressCount));
  return true;
}

void BluetoothSettingsActivity::renderRemoteTestBox(const int boxTop, const int boxHeight) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int boxLeft = metrics.contentSidePadding;
  const int boxWidth = pageWidth - metrics.contentSidePadding * 2;

  renderer.drawRoundedRect(boxLeft, boxTop, boxWidth, boxHeight, 1, 6, true);

  // A press counts as "just now" for a few seconds. E-ink can't animate, so the
  // count is what proves a second press registered when the text doesn't change.
  static constexpr unsigned long RECENT_PRESS_MS = 4000;
  const bool recent = remotePressCount > 0 && (millis() - lastRemotePressAtMs) <= RECENT_PRESS_MS;

  std::string message;
  if (!btMgr || !btMgr->isEnabled()) {
    message = tr(STR_BT_TEST_NEEDS_ON);
  } else if (remotePressCount == 0) {
    // Also the reconnect hint: a sleeping remote only advertises once a button
    // is pressed, so this is exactly what the user should do either way.
    message = tr(STR_BT_TEST_WAITING);
  } else {
    // tr() is a macro over a StrId name, so pick the id first.
    const StrId id = recent ? StrId::STR_BT_TEST_PRESSED : StrId::STR_BT_TEST_PRESSED_EARLIER;
    char buf[64];
    snprintf(buf, sizeof(buf), I18n::getInstance().get(id), static_cast<unsigned>(remotePressCount));
    message = buf;
  }

  const int textY = boxTop + (boxHeight - lineH) / 2;
  const std::string line =
      renderer.truncatedText(UI_10_FONT_ID, message.c_str(), boxWidth - metrics.contentSidePadding * 2);
  renderer.drawCenteredText(UI_10_FONT_ID, textY, line.c_str(), recent, EpdFontFamily::BOLD);
}

void BluetoothSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (viewMode == ViewMode::MAIN_MENU) {
    renderMainMenu();
  } else if (viewMode == ViewMode::MAP_BACK || viewMode == ViewMode::MAP_FORWARD) {
    renderMapping();
  } else {
    drawScanHeader();
    switch (viewMode) {
      case ViewMode::SCANNING:
        renderScanning();
        break;
      case ViewMode::DEVICE_LIST:
        renderDeviceList();
        break;
      case ViewMode::CONNECTING:
        renderConnecting();
        break;
      case ViewMode::CONNECTED:
        renderConnected();
        break;
      case ViewMode::CONNECT_FAILED:
        renderConnectFailed();
        break;
      case ViewMode::MAIN_MENU:
      case ViewMode::MAP_BACK:
      case ViewMode::MAP_FORWARD:
        break;
    }
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

// List body of the current view, between the sub-header and the button hints.
// Shared by render() and the loop()'s tap hit-testing so the two can never
// disagree. DEVICE_LIST reserves one extra verticalSpacing at the bottom for
// the legend help text.
Rect BluetoothSettingsActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (viewMode == ViewMode::DEVICE_LIST) {
    contentHeight -= metrics.verticalSpacing;
  }
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void BluetoothSettingsActivity::renderMainMenu() {
  auto metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Header with Bluetooth title
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER));

  // Status subheader
  std::string statusLine;
  if (btMgr) {
    if (!btMgr->isEnabled()) {
      statusLine = tr(STR_BT_IS_OFF);
    } else if (SETTINGS.bleBondedDeviceAddr[0] != '\0' && btMgr->isConnected(SETTINGS.bleBondedDeviceAddr)) {
      statusLine = std::string(tr(STR_BT_CONNECTED_TO)) + ": " +
                   (SETTINGS.bleBondedDeviceName[0] ? SETTINGS.bleBondedDeviceName : tr(STR_BT_A_REMOTE));
    } else if (SETTINGS.bleBondedDeviceAddr[0] != '\0') {
      statusLine = tr(STR_BT_REMOTE_NOT_CONNECTED);
    } else {
      statusLine = tr(STR_BT_NO_REMOTE_PAIRED);
    }
  } else {
    statusLine = tr(STR_BT_INIT_ERROR);
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
    // Reconnect + mapping rows — must stay index-aligned with kReconnectIndex /
    // kMapButtonsIndex in handleMainMenuInput(). The reconnect row is always
    // present while bonded: connection state changes asynchronously (the
    // background scan can reconnect mid-screen), and a row that comes and goes
    // with it would shift the indices between paint and press.
    itemLabels.push_back(tr(STR_BT_RECONNECT));
    itemValues.push_back(btMgr && btMgr->isConnected(SETTINGS.bleBondedDeviceAddr) ? tr(STR_CONNECTED) : "");
    itemLabels.push_back(tr(STR_BT_MAP_BUTTONS));
    itemValues.push_back(SETTINGS.bleBackSigIndex != 0xFF ? tr(STR_BT_MAP_TWO_BUTTON) : tr(STR_BT_MAP_ONE_BUTTON));
  } else {
    itemLabels.push_back(tr(STR_BT_NO_REMOTE_CONNECT));
    itemValues.push_back("");
  }

  GUI.drawList(
      renderer, listRect(), static_cast<int>(itemLabels.size()), selectedIndex,
      [&itemLabels](int index) { return itemLabels[index]; }, nullptr, nullptr,
      [&itemValues](int i) { return i < (int)itemValues.size() ? itemValues[i] : std::string(""); }, true);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int statusY = pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - lineH;

  // Remote test box, directly above the status line: press a button on the
  // remote and this confirms the device is receiving it — without having to
  // leave Settings and open a book to find out.
  const int testBoxHeight = lineH * 2;
  const int testBoxTop = statusY - testBoxHeight - metrics.verticalSpacing;
  renderRemoteTestBox(testBoxTop, testBoxHeight);

  // Expectation-setting: this feature is experimental and trades RAM for the
  // remote, so tell the user up-front why Bluetooth keeps turning itself off.
  {
    const auto explainerLines =
        renderer.wrappedText(UI_10_FONT_ID, tr(STR_BT_EXPLAINER), pageWidth - metrics.contentSidePadding * 2, 8);
    const int explainerY = testBoxTop - static_cast<int>(explainerLines.size()) * lineH - metrics.verticalSpacing;
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
}

void BluetoothSettingsActivity::drawScanHeader() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  // A bonded reconnect never ran a scan, so the discovered-device count (and
  // "No devices found") would be misleading over its connecting/result screens.
  if (connectingBonded) {
    const char* subheader = tr(STR_CONNECTING);
    if (viewMode == ViewMode::CONNECTED) {
      subheader = tr(STR_CONNECTED);
    } else if (viewMode == ViewMode::CONNECT_FAILED) {
      subheader = tr(STR_BT_ERR_CONNECT);
    }
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER));
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                      subheader);
    return;
  }

  const size_t deviceCount = btMgr ? btMgr->getDiscoveredDevices().size() : 0;

  char countStr[32];
  snprintf(countStr, sizeof(countStr), tr(STR_BT_DEVICES_FOUND), deviceCount);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER),
                 countStr);

  // Subheader carries the state of the search, the way the WiFi picker keeps
  // the adapter line under its header.
  const char* subheader = tr(STR_BT_SEARCHING);
  if (!btMgr) {
    subheader = tr(STR_BT_ERROR);
  } else if (viewMode != ViewMode::SCANNING) {
    subheader = deviceCount == 0 ? tr(STR_BT_NO_DEVICES) : tr(STR_BT_SELECT_REMOTE);
  }
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    subheader);
}

void BluetoothSettingsActivity::renderScanning() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, tr(STR_SCANNING), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, top + 10, tr(STR_BT_PRESS_REMOTE_BUTTON));

  // Back is the only action while scanning, so it's the only hint shown.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderDeviceList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& devices = btMgr->getDiscoveredDevices();

  if (devices.empty()) {
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = (pageHeight - height) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BT_NO_DEVICES));
    renderer.drawCenteredText(SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  } else {
    GUI.drawList(
        renderer, listRect(), static_cast<int>(devices.size()), selectedIndex,
        [&devices](int index) { return devices[index].name; }, nullptr, nullptr,
        [this, &devices](int index) {
          const auto& device = devices[index];
          return std::string(btMgr->isConnected(device.address) ? "* " : "") + (device.isHID ? "+ " : "") +
                 getSignalStrengthIndicator(device.rssi);
        });
  }

  GUI.drawHelpText(renderer,
                   Rect{0, pageHeight - metrics.buttonHintsHeight - metrics.contentSidePadding - 15, pageWidth, 20},
                   tr(STR_BT_DEVICE_LEGEND));

  // Third button appears only when there is something to disconnect, mirroring
  // the way the WiFi list only offers Forget on a saved network.
  const char* disconnectLabel = btMgr->getConnectedDevices().empty() ? "" : tr(STR_BT_DISCONNECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), disconnectLabel, tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderConnecting() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, tr(STR_CONNECTING), true, EpdFontFamily::BOLD);
  if (!selectedDeviceName.empty()) {
    const std::string deviceInfo = std::string(tr(STR_BT_DEVICE_PREFIX)) + selectedDeviceName;
    renderer.drawCenteredText(UI_10_FONT_ID, top + 10, deviceInfo.c_str());
  }
}

void BluetoothSettingsActivity::renderConnected() const {
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 4) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);

  const std::string deviceInfo = std::string(tr(STR_BT_DEVICE_PREFIX)) + selectedDeviceName;
  renderer.drawCenteredText(UI_10_FONT_ID, top + 10, deviceInfo.c_str());
  renderer.drawCenteredText(UI_10_FONT_ID, top + 40, tr(STR_BT_REMOTE_REMEMBERED));
  if (guidedSetup) {
    // Guided setup continues into the mapping wizard, so set the expectation
    // here and label the confirm hint Next rather than Done.
    renderer.drawCenteredText(UI_10_FONT_ID, top + 70, tr(STR_BT_SETUP_NEXT_MAP));
  }

  const auto labels = mappedInput.mapLabels("", guidedSetup ? tr(STR_NEXT) : tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderConnectFailed() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 4) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 30, tr(STR_BT_ERR_CONNECT), true, EpdFontFamily::BOLD);

  if (!selectedDeviceName.empty()) {
    const std::string deviceInfo = std::string(tr(STR_BT_DEVICE_PREFIX)) + selectedDeviceName;
    renderer.drawCenteredText(UI_10_FONT_ID, top + 10, deviceInfo.c_str());
  }
  if (!connectionError.empty()) {
    // The manager's reason is the useful part — wrap it rather than truncate,
    // since several of these are full sentences.
    const auto lines =
        renderer.wrappedText(UI_10_FONT_ID, connectionError.c_str(), pageWidth - metrics.contentSidePadding * 4, 3);
    for (int i = 0; i < static_cast<int>(lines.size()); i++) {
      renderer.drawCenteredText(UI_10_FONT_ID, top + 40 + i * height, lines[i].c_str());
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BluetoothSettingsActivity::renderMapping() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BT_PAGE_TURNER));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_BT_MAP_BUTTONS));

  const char* stepTitle = viewMode == ViewMode::MAP_BACK ? tr(STR_BT_MAP_PRESS_BACK) : tr(STR_BT_MAP_PRESS_FORWARD);
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, stepTitle, pageWidth - metrics.contentSidePadding * 4, 3);
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int top = (pageHeight - static_cast<int>(titleLines.size()) * titleLineH) / 2 - lineH;
  for (int i = 0; i < static_cast<int>(titleLines.size()); i++) {
    renderer.drawCenteredText(UI_12_FONT_ID, top + i * titleLineH, titleLines[i].c_str(), true, EpdFontFamily::BOLD);
  }

  // Two matching presses lock each button in, so the sub-line tracks whether we
  // are waiting for the first press or the confirming one.
  const char* prompt = mapStepPresses == 0 ? tr(STR_BT_MAP_PRESS_TWICE) : tr(STR_BT_MAP_PRESS_AGAIN);
  renderer.drawCenteredText(UI_10_FONT_ID, top + static_cast<int>(titleLines.size()) * titleLineH + lineH, prompt);

  // During guided setup this step is optional, so Back reads as Skip.
  const auto labels = mappedInput.mapLabels(guidedSetup ? tr(STR_SKIP) : tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
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
