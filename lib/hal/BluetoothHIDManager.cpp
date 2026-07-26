#include "BluetoothHIDManager.h"

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#if defined(ARDUINO) && __has_include(<esp32-hal-bt-mem.h>)
// Arduino-ESP32 3.x releases BT controller memory during startup unless a
// Bluetooth library marks it as in use before app_main(). NimBLE-Arduino does
// not do that automatically in this build, which can crash later in
// `NimBLEDevice::init()` / `esp_bt_controller_init()` when Bluetooth is enabled
// from the settings UI on ESP32-C3. Pulling in this header sets the core's
// `_btLibraryInUse` flag early via a constructor and keeps BLE memory reserved.
#include <esp32-hal-bt-mem.h>
#endif

// HID Service and characteristic UUIDs
static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2A4D";
static const char* HID_INFO_UUID = "2A4A";
static const char* HID_PROTOCOL_MODE_UUID = "2A4E";

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
// Keep latency at 0 for low input lag while allowing a longer supervision timeout
// to reduce disconnects at marginal range.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;  // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;  // 30ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_TIMEOUT = 600;  // 6s
constexpr uint16_t BLE_CONN_SCAN_INTERVAL = 60;
constexpr uint16_t BLE_CONN_SCAN_WINDOW = 30;
constexpr uint32_t BLE_CONNECT_TIMEOUT_MS = 10000;
// Floor below which enabling is not worth attempting.
//
// This is deliberately NOT the controller's real requirement, because that value
// is not known. Anecdotally it wants "about 30KB contiguous", but that came from
// prose, not measurement — and a 30KB gate was observed refusing at 29,684 on a
// device that had enabled successfully moments earlier. A hard gate on a guessed
// threshold turns a working feature into a broken one.
//
// So: refuse only when the request is hopeless by any reading, and let
// NimBLEDevice::init() (whose return value IS checked) report real failures. Every
// attempt logs the largest block, so the true threshold can be established from
// evidence rather than assumed.
constexpr uint32_t BLE_CONTROLLER_MIN_BLOCK = 20 * 1024;
// --- Press detector tuning ---
// Every button on every remote means "next page", so nothing here decodes
// keycodes. The detector only has to answer "did a button just go down?".
constexpr unsigned long BASELINE_LEARN_MS = 1000;     // Post-connect window to learn the idle report
constexpr uint8_t VOLATILE_CHANGE_THRESHOLD = 3;      // Byte changes in a window => free-running, mask it
constexpr unsigned long MIN_PRESS_INTERVAL_MS = 120;  // Floor between injected page turns
constexpr unsigned long STUCK_ACTIVE_MS = 2500;       // Active this long with churning bytes => re-learn

// RAII flag: while alive, tells pauseForMemory() that loop-task BLE work is in
// flight so it waits (or skips) instead of tearing the stack down underneath us.
// Used by updateActivity() and checkAutoReconnect().
struct MaintenanceBusyScope {
  volatile bool& flag;
  explicit MaintenanceBusyScope(volatile bool& f) : flag(f) { flag = true; }
  ~MaintenanceBusyScope() { flag = false; }
};
}  // namespace

// Global static for singleton
static BluetoothHIDManager* g_instance = nullptr;

// Scan callbacks for NimBLE 2.x - keep as static to ensure it stays alive
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      // onScanResult expects non-const pointer, need to cast
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    } else {
      LOG_ERR("BT", "onResult called but g_instance is NULL!");
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results;
    (void)reason;
  }
};

// Static instance to keep callbacks alive during scan
static ScanCallbacks scanCallbacks;

// Client connection callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pClient) override {
    LOG_INF("BT", "Client connected: %s", pClient->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient* pClient, int reason) override {
    LOG_ERR("BT", "Client disconnected: %s (reason: %d)", pClient->getPeerAddress().toString().c_str(), reason);
  }
};

BluetoothHIDManager& BluetoothHIDManager::getInstance() {
  if (!g_instance) {
    g_instance = new BluetoothHIDManager();
    LOG_INF("BT", "BluetoothHIDManager instance created");
  }
  return *g_instance;
}

BluetoothHIDManager::BluetoothHIDManager() { LOG_DBG("BT", "BluetoothHIDManager constructor"); }

BluetoothHIDManager::~BluetoothHIDManager() { cleanup(); }

void BluetoothHIDManager::cleanup() {
  if (_enabled) {
    disable();
  }
}

bool BluetoothHIDManager::enable() {
  if (_enabled) {
    LOG_DBG("BT", "Already enabled");
    return true;
  }

  LOG_INF("BT", "Enabling Bluetooth...");

  // Starting the BT controller + NimBLE host consumes ~56KB of heap in several
  // chunks (measured on-device); attempting it with less can hang inside
  // controller init rather than fail, and succeeding into a near-empty heap
  // OOM-aborts on the next render. Require the stack's cost plus working
  // slack, and refuse politely so the UI can tell the user.
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t largestBlock = ESP.getMaxAllocHeap();

  // BOTH checks are load-bearing, and the second is the one that actually bites.
  // The controller needs a single >=30KB CONTIGUOUS block. Once the BLE stack has
  // run and been torn down, ~19 small allocations survive scattered through the
  // working region and cap the largest block near 25KB while total free still
  // reads ~99KB — so a free-bytes-only gate waves through an attempt that then
  // HANGS inside controller init. Refusing here turns a dead device into a
  // message. Measured on hardware: free=99316 / maxAlloc=26612 after one BLE
  // session, i.e. comfortably past the free-heap gate and unable to start.
  if (freeHeap < 75000) {
    LOG_ERR("BT", "Not enough free heap to enable Bluetooth (%u bytes)", freeHeap);
    lastError = "Not enough free memory";
    lastStatus = BtStatus::NotEnoughMemory;
    return false;
  }
  if (largestBlock < BLE_CONTROLLER_MIN_BLOCK) {
    LOG_ERR("BT", "Heap too fragmented to enable Bluetooth (largest block %u, floor %u; free %u)", largestBlock,
            static_cast<unsigned>(BLE_CONTROLLER_MIN_BLOCK), freeHeap);
    lastError = "Memory too fragmented - restart to use Bluetooth";
    lastStatus = BtStatus::HeapFragmented;
    return false;
  }
  // Above the floor we attempt regardless. Log the block size on every attempt so
  // successes and failures can be correlated against it and the real requirement
  // learned; init()'s return value catches a clean failure.
  LOG_INF("BT", "Enable attempt: heap %u, largest block %u", freeHeap, largestBlock);

  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi to enable Bluetooth (mutual exclusion)");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }

  // Initialize NimBLE stack. The return value matters: without it a failed init
  // was reported as "Bluetooth enabled successfully" and every later call worked
  // against a stack that was never up.
  if (!NimBLEDevice::init("CrossPoint")) {
    LOG_ERR("BT", "NimBLEDevice::init failed (heap %u, largest %u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    lastError = "Bluetooth failed to start";
    lastStatus = BtStatus::StartFailed;
    return false;
  }
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9dBm
  NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_1M_MASK);
  NimBLEDevice::setSecurityAuth(true, false, true);

  _enabled = true;
  lastError = "";
  lastStatus = BtStatus::None;

  LOG_INF("BT", "Bluetooth enabled successfully (heap %u, largest %u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  loadState();
  return true;
}

bool BluetoothHIDManager::disable() {
  if (!_enabled) {
    LOG_DBG("BT", "Already disabled");
    return true;
  }

  LOG_INF("BT", "Disabling Bluetooth...");

  stopBackgroundScan();
  _pendingBondedConnect = false;
  if (_scanning) {
    stopScan();
  }

  // Disconnect all devices
  while (!_connectedDevices.empty()) {
    disconnectFromDevice(_connectedDevices[0].address);
  }

  // Deinitialize NimBLE and free EVERYTHING it holds (clearAll=true). deinit(false)
  // keeps client objects and internals alive for fast re-init, but those small
  // survivors sit in the middle of the ~56KB the stack returns and cap the largest
  // contiguous free block at ~20KB — below the 2x16KB a TLS session needs on this
  // framework (MBEDTLS_SSL_MAX_CONTENT_LEN=16384, fixed in the precompiled libs).
  // Reconnect always goes through the fresh-client path, so nothing needs to survive.
  NimBLEDevice::deinit(true);

  // Return the discovery/connection bookkeeping capacity too (clear() keeps it).
  std::vector<BluetoothDevice>().swap(_discoveredDevices);
  std::vector<ConnectedDevice>().swap(_connectedDevices);

  _enabled = false;
  lastError = "";

  LOG_INF("BT", "Bluetooth disabled (heap: %u, max alloc: %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  return true;
}

void BluetoothHIDManager::startScan(uint32_t durationMs) {
  if (!_enabled || _scanning) {
    LOG_DBG("BT", "Cannot scan: enabled=%d scanning=%d", _enabled, _scanning);
    return;
  }

  LOG_INF("BT", "Starting BLE scan for %lu ms", durationMs);
  stopBackgroundScan();
  _scanning = true;
  _discoveredDevices.clear();

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    LOG_ERR("BT", "Failed to get scan object");
    _scanning = false;
    lastError = "Scan failed";
    lastStatus = BtStatus::ScanFailed;
    return;
  }

  // Use static callbacks object to ensure it stays alive
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  // We keep our own _discoveredDevices list, so NimBLE's parallel cache of
  // NimBLEAdvertisedDevice objects is pure waste — and in a busy RF environment
  // it grows unbounded for the whole scan. 0 = don't retain results.
  pScan->setMaxResults(0);

  // In NimBLE 2.x, duration=0 means scan continuously until stop() is called
  // Parameter 1: 0 = continuous scan
  // Parameter 2: isContinue (false = clear old results)
  bool started = pScan->start(0, false);

  if (!started) {
    LOG_ERR("BT", "Failed to start scan!");
    _scanning = false;
    lastError = "Scan failed";
    lastStatus = BtStatus::ScanFailed;
    return;
  }

  // Wait for the specified duration
  delay(durationMs);

  // Stop the scan
  pScan->stop();

  _scanning = false;
  LOG_INF("BT", "Scan complete, found %d devices", _discoveredDevices.size());
}

void BluetoothHIDManager::stopScan() {
  if (!_scanning) return;

  LOG_INF("BT", "Stopping scan");

  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan) {
    pScan->stop();
  }

  _scanning = false;
}

void BluetoothHIDManager::startBackgroundScan() {
  if (!_enabled || _scanning || _backgroundScanActive) {
    return;
  }
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan) {
    return;
  }
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(false);  // passive: the advertiser's address is all we need
  // A cheap HID clicker (e.g. AB Shutter3) only advertises in a short burst when
  // one of its buttons is pressed, then goes quiet again. At ~5% radio duty
  // (50ms every 1000ms) those bursts almost always land in the 95% the radio
  // isn't listening, so a press-to-wake reconnect silently fails. Listen nearly
  // continuously instead (90ms window every 100ms, ~90% duty) so a press is
  // caught within one advertising interval. This only runs while the remote is
  // disconnected, and BLE's own inactivity timeout still powers the stack down
  // after a few minutes idle, so the higher drain is bounded to active use.
  pScan->setInterval(160);  // 100ms interval...
  pScan->setWindow(144);    // ...with a 90ms window: ~90% radio duty while reconnecting
  // This scan runs for as long as the remote stays disconnected — possibly the
  // whole reading session — so retaining results would leak steadily. We only
  // compare addresses in the callback and keep nothing.
  pScan->setMaxResults(0);
  if (pScan->start(0, false)) {
    _backgroundScanActive = true;
    LOG_DBG("BT", "Background reconnect scan started");
  }
}

void BluetoothHIDManager::stopBackgroundScan() {
  if (!_backgroundScanActive) {
    return;
  }
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (pScan) {
    pScan->stop();
  }
  _backgroundScanActive = false;
  LOG_DBG("BT", "Background reconnect scan stopped");
}

void BluetoothHIDManager::onScanResult(NimBLEAdvertisedDevice* advertisedDevice) {
  if (!advertisedDevice) return;

  std::string address = advertisedDevice->getAddress().toString();

  // Background reconnect mode: don't record devices — just watch for the bonded
  // remote waking up (it advertises after one of its buttons is pressed).
  if (_backgroundScanActive) {
    if (!_bondedDeviceAddress.empty() && address == _bondedDeviceAddress) {
      LOG_INF("BT", "Bonded remote is advertising, scheduling reconnect");
      NimBLEScan* pScan = NimBLEDevice::getScan();
      if (pScan) {
        pScan->stop();
      }
      _backgroundScanActive = false;
      _pendingBondedConnect = true;
    }
    return;
  }

  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();

  // Check if device advertises HID service
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));

  // Check if we already have this device
  for (auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      dev.rssi = rssi;  // Update RSSI
      if (isHID) dev.isHID = true;
      // The name often arrives in a separate scan-response packet after the first
      // sighting — back-fill it so an HID device doesn't stay listed as "Unknown".
      if (!name.empty() && dev.name == "Unknown") {
        dev.name = name;
      }
      return;
    }
  }

  // Skip nameless devices that don't advertise HID: they are phones, beacons and
  // earbuds that flood the list as "Unknown" and can't be a page turner we could
  // use. Nameless HID devices stay visible (some clickers advertise no name), and
  // if the name shows up in a later scan response the back-fill above catches it.
  if (name.empty() && !isHID) {
    LOG_DBG("BT", "Skipping unnamed non-HID device %s RSSI:%d", address.c_str(), rssi);
    return;
  }

  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.isHID = isHID;

  _discoveredDevices.push_back(device);

  LOG_DBG("BT", "Found device: %s (%s) RSSI:%d HID:%d", device.name.c_str(), device.address.c_str(), rssi, isHID);
}

bool BluetoothHIDManager::connectToDevice(const std::string& address) {
  if (!_enabled) {
    LOG_ERR("BT", "Cannot connect: Bluetooth not enabled");
    lastError = "Bluetooth not enabled";
    lastStatus = BtStatus::NotEnabled;
    return false;
  }

  // Check if already connected
  if (isConnected(address)) {
    LOG_INF("BT", "Already connected to %s", address.c_str());
    return true;
  }

  LOG_INF("BT", "Connecting to device %s", address.c_str());
  stopBackgroundScan();

  NimBLEAddress bleAddress(address, BLE_ADDR_PUBLIC);

  // Reuse existing disconnected client objects to avoid NimBLE deleteClient() on this target.
  NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(bleAddress);
  const bool hadExistingClient = (pClient != nullptr);
  if (!pClient) {
    pClient = NimBLEDevice::getDisconnectedClient();
    if (pClient) {
      pClient->setPeerAddress(bleAddress);
    }
  }
  if (!pClient) {
    pClient = NimBLEDevice::createClient(bleAddress);
  }

  if (!pClient) {
    lastError = "Failed to create BLE client";
    lastStatus = BtStatus::ClientFailed;
    LOG_ERR("BT", "Failed to create BLE client");
    return false;
  }

  // Keep client lifetime under manager control so disconnect callbacks do not free it in NimBLE context.
  pClient->setSelfDelete(false, false);
  pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
  pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                               BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);

  if (!pClient->isConnected()) {
    pClient->deleteServices();
  }

  // Set connection callbacks
  static ClientCallbacks clientCallbacks;
  pClient->setClientCallbacks(&clientCallbacks, false);

  // Connect to device
  if (!pClient->connect(bleAddress)) {
    if (hadExistingClient) {
      LOG_INF("BT", "Reconnect with existing client failed for %s, retrying with fresh client", address.c_str());
      NimBLEClient* freshClient = NimBLEDevice::createClient(bleAddress);
      if (freshClient) {
        pClient = freshClient;
        pClient->setSelfDelete(false, false);
        pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
        pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT,
                                     BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
        pClient->setClientCallbacks(&clientCallbacks, false);
      }
    }

    if (!pClient->connect(bleAddress)) {
      lastError = "Connection failed";
      lastStatus = BtStatus::ConnectFailed;
      LOG_ERR("BT", "Failed to connect to %s", address.c_str());
      return false;
    }
  }

  const bool connParamsUpdated =
      pClient->updateConnParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
  LOG_INF("BT", "Connection params update request: %d", connParamsUpdated);

  const bool dataLenUpdated = pClient->setDataLen(251);
  LOG_INF("BT", "Data length extension request (251): %d", dataLenUpdated);

  const int connectedRssi = pClient->getRssi();
  LOG_INF("BT", "Connected RSSI for %s: %d dBm", address.c_str(), connectedRssi);

  // Get HID service
  NimBLERemoteService* pService = pClient->getService(HID_SERVICE_UUID);
  if (!pService) {
    lastError = "HID service not found";
    lastStatus = BtStatus::NoHidService;
    LOG_ERR("BT", "Device %s doesn't have HID service", address.c_str());
    pClient->disconnect();
    return false;
  }

  // Attempt to force Report Protocol mode (0x01) when supported.
  // Some remotes behave inconsistently unless protocol mode is explicit.
  if (auto* pProtocolMode = pService->getCharacteristic(HID_PROTOCOL_MODE_UUID)) {
    if (pProtocolMode->canWrite() || pProtocolMode->canWriteNoResponse()) {
      uint8_t reportMode = 0x01;
      const bool protocolSet = pProtocolMode->writeValue(&reportMode, 1, false);
      LOG_INF("BT", "Protocol mode write (Report=0x01): %d", protocolSet);
    }
  }

  LOG_INF("BT", "Found HID service, enumerating report characteristics...");

  // BLE HID has multiple report characteristics (input, output, feature)
  // We need to find one that supports NOTIFY or INDICATE (input report)
  // In NimBLE 2.x, getCharacteristics() returns std::vector<NimBLERemoteCharacteristic*>
  auto pCharacteristics = pService->getCharacteristics(true);
  NimBLERemoteCharacteristic* pReportChar = nullptr;

  int reportCount = 0;
  std::vector<NimBLERemoteCharacteristic*> reportChars;

  for (auto it = pCharacteristics.begin(); it != pCharacteristics.end(); ++it) {
    auto* pChar = *it;
    LOG_DBG("BT", "Characteristic UUID: %s, canRead:%d canWrite:%d canNotify:%d canIndicate:%d",
            pChar->getUUID().toString().c_str(), pChar->canRead(), pChar->canWrite(), pChar->canNotify(),
            pChar->canIndicate());

    if (pChar->getUUID().equals(NimBLEUUID(HID_REPORT_UUID))) {
      reportCount++;

      // Check if this report supports notify or indicate (input report)
      if (pChar->canNotify() || pChar->canIndicate()) {
        reportChars.push_back(pChar);
        LOG_INF("BT", "Added Report char #%d for subscription", reportCount);
      }
    }
  }

  if (reportChars.empty()) {
    lastError = "No input report characteristic found";
    lastStatus = BtStatus::NoReportChar;
    LOG_ERR("BT", "No Report characteristic with notify/indicate found");
    pClient->disconnect();
    return false;
  }

  // Subscribe to ALL Report characteristics with notify capability
  LOG_INF("BT", "Subscribing to %d Report characteristics...", reportChars.size());
  size_t successfulSubscriptions = 0;

  for (size_t i = 0; i < reportChars.size(); i++) {
    auto* pChar = reportChars[i];

    // Clear stale CCCD state on reused clients where possible.
    (void)pChar->unsubscribe();

    // Use notifications when available, otherwise indications.
    const bool useNotify = pChar->canNotify();
    bool subResult = pChar->subscribe(useNotify, onHIDNotify);
    LOG_INF("BT", "Report char #%d subscribe (%s) result: %d", i + 1, useNotify ? "notify" : "indicate", subResult);
    if (subResult) {
      successfulSubscriptions++;
    }

    if (!subResult) {
      LOG_INF("BT", "Failed to subscribe to Report char #%d (continuing)", i + 1);
    }
  }

  if (successfulSubscriptions == 0) {
    lastError = "Failed to subscribe to input reports";
    lastStatus = BtStatus::SubscribeFailed;
    LOG_ERR("BT", "No HID report subscriptions succeeded for %s", address.c_str());
    pClient->disconnect();
    return false;
  }

  LOG_INF("BT", "Subscribed to %u/%u HID Report characteristics", static_cast<unsigned>(successfulSubscriptions),
          static_cast<unsigned>(reportChars.size()));

  // Save connection with activity timestamp
  ConnectedDevice connDev;
  connDev.address = address;
  connDev.client = pClient;
  connDev.reportChars = reportChars;
  connDev.connectedTime = millis();
  connDev.subscribed = true;
  connDev.lastActivityTime = millis();  // Initialize activity timer
  connDev.wasConnected = true;          // Mark for auto-reconnect if disconnected

  // Try to find the device in scan results to get its name (display only)
  bool foundInScan = false;
  for (const auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      connDev.name = dev.name;
      foundInScan = true;
      LOG_INF("BT", "Device found in scan results: %s (%s)", dev.name.c_str(), address.c_str());
      break;
    }
  }

  if (!foundInScan) {
    LOG_INF("BT", "Device not in scan results (may be previously paired): %s", address.c_str());
    if (connDev.name.empty() && !_bondedDeviceAddress.empty() && _bondedDeviceAddress == address &&
        !_bondedDeviceName.empty()) {
      connDev.name = _bondedDeviceName;
      LOG_INF("BT", "Using bonded device name hint: %s", connDev.name.c_str());
    }
  }

  // No profile lookup: every button on every remote turns one page forward, so
  // the press detector learns this remote's idle report at runtime instead of
  // matching it against a keycode database. See detectPress().
  LOG_INF("BT", "Learning idle report for %s (%lu ms)", address.c_str(), BASELINE_LEARN_MS);

  auto existing = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                               [&address](const ConnectedDevice& dev) { return dev.address == address; });
  if (existing != _connectedDevices.end()) {
    *existing = connDev;
  } else {
    _connectedDevices.push_back(connDev);
  }

  // The scan list has served its purpose (name lookup, above). Hand its memory
  // back rather than carrying one heap-allocated address+name string per nearby
  // device for the rest of the reading session — clear() would keep the capacity.
  std::vector<BluetoothDevice>().swap(_discoveredDevices);

  LOG_INF("BT", "Successfully connected to %s", address.c_str());
  lastError = "Connected";
  lastStatus = BtStatus::Connected;
  return true;
}

bool BluetoothHIDManager::disconnectFromDevice(const std::string& address) {
  LOG_INF("BT", "Disconnecting from device %s", address.c_str());

  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                         [&address](const ConnectedDevice& dev) { return dev.address == address; });

  if (it != _connectedDevices.end()) {
    NimBLEClient* client = it->client;

    // Ensure normal CPU speed during BLE termination to avoid WDT in low-power mode.
    if (client && client->isConnected()) {
      HalPowerManager::Lock lock;
      client->disconnect();
      // BLE termination is asynchronous. Wait briefly for the link to actually
      // drop so a following NimBLE deinit doesn't race it (observed as
      // "ble_hs_stop: failed to terminate connection; rc=2" and can leave the
      // shared radio in a state that degrades the WiFi that follows).
      const unsigned long start = millis();
      while (client->isConnected() && millis() - start < 700) {
        delay(20);
      }
    }

    // Remove from our list
    _connectedDevices.erase(it);
    LOG_INF("BT", "Disconnected from %s", address.c_str());
    return true;
  }

  LOG_INF("BT", "Device %s not in connected list", address.c_str());
  return false;
}

bool BluetoothHIDManager::isConnected(const std::string& address) const {
  return std::find_if(_connectedDevices.begin(), _connectedDevices.end(), [&address](const ConnectedDevice& dev) {
           return dev.address == address && dev.client && dev.client->isConnected();
         }) != _connectedDevices.end();
}

std::vector<std::string> BluetoothHIDManager::getConnectedDevices() const {
  std::vector<std::string> addresses;
  for (const auto& dev : _connectedDevices) {
    if (dev.client && dev.client->isConnected()) {
      addresses.push_back(dev.address);
    }
  }
  return addresses;
}

ConnectedDevice* BluetoothHIDManager::findConnectedDevice(const std::string& address) {
  auto it = std::find_if(_connectedDevices.begin(), _connectedDevices.end(),
                         [&address](const ConnectedDevice& dev) { return dev.address == address; });

  if (it != _connectedDevices.end()) {
    return &(*it);
  }
  return nullptr;
}

void BluetoothHIDManager::processInputEvents() {
  // Input events are processed via notifications callback
  // This method is kept for potential polling-based implementations
}

bool BluetoothHIDManager::pauseForMemory() {
  if (!_enabled || _memoryPaused) {
    return false;
  }

  // Ask the loop task's maintenance to stand down, then wait for any in-flight
  // pass to finish. If a (blocking) bonded reconnect is running we skip the
  // pause entirely rather than tear NimBLE down underneath it.
  _maintenanceSuspended = true;
  const unsigned long start = millis();
  while (_maintenanceBusy && millis() - start < 500) {
    delay(10);
  }
  if (_maintenanceBusy) {
    _maintenanceSuspended = false;
    LOG_INF("BT", "Memory pause skipped: maintenance/reconnect in flight");
    return false;
  }

  // Set before disable() so the loop task's lazy restore can't re-enable the
  // stack in the middle of the memory-critical section.
  _memoryPaused = true;
  LOG_INF("BT", "Pausing BLE for memory-critical section (heap before: %u)", ESP.getFreeHeap());
  disable();
  LOG_INF("BT", "BLE paused (heap after: %u, max alloc: %u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

void BluetoothHIDManager::endMemoryPause() {
  // Leaves the stack down: the reader's maybeAutoRestoreBluetooth() brings it back
  // once the heavy operation's heap is free again (the "reconnect on chapter load"
  // path), or the user re-enables it via a toggle.
  _memoryPaused = false;
  _maintenanceSuspended = false;
  LOG_INF("BT", "Memory pause ended (auto-restore will re-enable when heap allows)");
}

void BluetoothHIDManager::setBluetoothWanted(bool wanted) {
  if (_bluetoothWanted == wanted) {
    return;
  }
  _bluetoothWanted = wanted;
  if (wanted) {
    // Arm an immediate first restore attempt (clear the rate-limit clock).
    _lastRestoreAttemptMs = 0;
  }
  LOG_INF("BT", "Bluetooth %s", wanted ? "wanted (auto-restore armed)" : "no longer wanted (auto-restore off)");
}

void BluetoothHIDManager::noteWifiActivity() {
  _lastWifiActivityMs = millis();
  if (_lastWifiActivityMs == 0) {
    _lastWifiActivityMs = 1;  // 0 is the "never" sentinel; keep a real timestamp
  }
}

bool BluetoothHIDManager::beginAutoRestoreAttempt() {
  // Pure gate + rate-limit stamp. Does not allocate, touch the radio, or enable —
  // the reader frees the chapter layout and calls enable() when this returns true.
  if (_enabled || !_bluetoothWanted) {
    return false;
  }
  // The render task is mid memory-critical-section (a section build). Setting
  // _maintenanceSuspended is how pauseForMemory() fences the loop task out; honour
  // it, and never restore while paused.
  if (_memoryPaused || _maintenanceSuspended) {
    return false;
  }
  const unsigned long now = millis();
  if (_lastRestoreAttemptMs != 0 && now - _lastRestoreAttemptMs < BLE_RESTORE_RETRY_MS) {
    return false;
  }
  // WiFi settle: refuse to bring the BT controller up until the chip has had
  // quiet time since the last WiFi teardown (see BLE_WIFI_SETTLE_MS).
  if (_lastWifiActivityMs != 0 && now - _lastWifiActivityMs < BLE_WIFI_SETTLE_MS) {
    return false;
  }
  _lastRestoreAttemptMs = now;
  LOG_INF("BT", "Auto-restore window open (heap %u, largest %u, %lums since WiFi) — reader will free layout + enable",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap(), _lastWifiActivityMs ? now - _lastWifiActivityMs : 0UL);
  return true;
}

void BluetoothHIDManager::setButtonInjector(std::function<void(uint8_t, bool)> injector) {
  _buttonInjector = injector;
  LOG_DBG("BT", "Button injector registered");
}

void BluetoothHIDManager::setReaderContextCallback(std::function<bool()> callback) {
  _readerContextCallback = callback;
  LOG_DBG("BT", "Reader context callback registered");
}

void BluetoothHIDManager::setBondedDevice(const std::string& address, const std::string& name) {
  _bondedDeviceAddress = address;
  _bondedDeviceName = name;
  LOG_INF("BT", "Bonded device set: %s (%s)", _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // Check if any connected device has had activity in the last 4 minutes
  // This prevents power sleep while using BLE controller
  unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime > 0) {
      unsigned long timeSinceActivity = now - device.lastActivityTime;
      if (timeSinceActivity < 240000) {  // 4 minute (240 second) threshold to keep BLE alive
        return true;
      }
    }
  }
  return false;
}

bool BluetoothHIDManager::hadRecentRemoteInput(unsigned long windowMs) const {
  const unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastRemoteInputMs != 0 && (now - device.lastRemoteInputMs) <= windowMs) {
      return true;
    }
  }
  return false;
}

unsigned long BluetoothHIDManager::lastRemoteInputMs() const {
  unsigned long latest = 0;
  for (const auto& device : _connectedDevices) {
    if (device.lastRemoteInputMs > latest) {
      latest = device.lastRemoteInputMs;
    }
  }
  return latest;
}

// --- Direction-agnostic press detection ---
//
// Page turners agree on nothing: keycodes, report layout, and even whether a
// button has a distinct code at all vary per model. Since every button now means
// "next page", none of that has to be decoded. The detector only needs the moment
// a button goes down, which it finds structurally rather than semantically:
//
//  1. For BASELINE_LEARN_MS after connect, record the report and count how often
//     each byte changes. Bytes that change VOLATILE_CHANGE_THRESHOLD times or
//     more are free-running (rolling counters, joystick axes, battery gauges);
//     they get masked out and never considered again.
//  2. The last frame of that window becomes the idle reference.
//  3. From then on, any unmasked byte differing from idle means a button is down.
//     The idle -> active edge is one page turn.
//
// Remotes that transmit only on press send too few frames to characterise, so the
// all-zero report is assumed idle for them — which is what a plain HID keyboard
// clicker reports anyway.
bool BluetoothHIDManager::detectPress(ConnectedDevice* device, const uint8_t* data, const size_t length,
                                      const unsigned long nowMs) {
  const size_t n = length < HID_FRAME_BYTES ? length : HID_FRAME_BYTES;
  uint8_t frame[HID_FRAME_BYTES] = {0};
  memcpy(frame, data, n);

  // Phase 1: learn this remote's idle report and its free-running bytes.
  if (!device->baselineReady) {
    if (device->baselineFrames == 0) {
      // First frame ever seen: it opens the window and seeds the reference.
      device->baselineStartMs = nowMs;
      device->baselineFrames = 1;
      memcpy(device->idleFrame, frame, HID_FRAME_BYTES);
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        if (frame[i] == 0) device->byteSeenZero |= static_cast<uint8_t>(1u << i);
      }
      return false;
    }

    if ((nowMs - device->baselineStartMs) < BASELINE_LEARN_MS) {
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        if (frame[i] != device->idleFrame[i] && device->byteChangeCount[i] < 0xFF) {
          device->byteChangeCount[i]++;
        }
        // A byte that visits 0x00 is resting between presses (a keycode), not a
        // free-running counter — remember that so we never mask it below.
        if (frame[i] == 0) device->byteSeenZero |= static_cast<uint8_t>(1u << i);
      }
      memcpy(device->idleFrame, frame, HID_FRAME_BYTES);
      if (device->baselineFrames < 0xFFFF) {
        device->baselineFrames++;
      }
      return false;
    }

    // Window closed. Note this frame is deliberately NOT folded into the
    // reference: on a remote that only transmits on press, it IS the press, and
    // adopting it as "idle" would invert the detector for the whole session.
    if (device->baselineFrames < 2) {
      // Silent remote: no idle traffic to characterise, so assume all-zero idle.
      memset(device->idleFrame, 0, HID_FRAME_BYTES);
    } else {
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        if (device->byteSeenZero & static_cast<uint8_t>(1u << i)) {
          // This byte returned to 0x00 during the window, so it is the remote's
          // signal byte (a keycode that rests at 0 between presses), NOT a
          // free-running counter. Its true idle is 0 — even if the window
          // happened to close on a press frame — and it must never be masked or
          // the detector goes blind. This is what keeps a press-only clicker
          // (AB Shutter3: byte0 = E9 pressed / 00 released) working even when
          // the user presses during the learn window.
          device->idleFrame[i] = 0;
        } else if (device->byteChangeCount[i] >= VOLATILE_CHANGE_THRESHOLD) {
          // Stayed non-zero on every frame AND churned: a real rolling counter.
          device->volatileMask |= static_cast<uint8_t>(1u << i);
        }
      }
    }
    memcpy(device->prevFrame, device->idleFrame, HID_FRAME_BYTES);
    device->baselineReady = true;
    LOG_INF("BT", "Idle report learned for %s: frames=%u volatileMask=0x%02X", device->address.c_str(),
            static_cast<unsigned>(device->baselineFrames), device->volatileMask);
    // Fall through: evaluate this frame normally so a press that arrives right
    // as the window closes still turns a page.
  }

  // Phase 2: does this frame differ from idle on any byte we still trust?
  bool active = false;
  for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
    if ((device->volatileMask & static_cast<uint8_t>(1u << i)) != 0) {
      continue;
    }
    if (frame[i] != device->idleFrame[i]) {
      active = true;
      break;
    }
  }

  if (!active) {
    device->active = false;
    device->activeSinceMs = 0;
    device->churnMask = 0;
    device->activeChangeCount = 0;
    memcpy(device->prevFrame, frame, HID_FRAME_BYTES);
    return false;
  }

  if (device->active) {
    // Already down. Either a real hold, or a counter byte we failed to mask
    // during learning. They are told apart by whether the bytes keep changing:
    // a held button's report is constant, a counter's is not.
    for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
      if (frame[i] != device->prevFrame[i]) {
        device->churnMask |= static_cast<uint8_t>(1u << i);
        if (device->activeChangeCount < 0xFF) {
          device->activeChangeCount++;
        }
        break;
      }
    }
    memcpy(device->prevFrame, frame, HID_FRAME_BYTES);

    // A remote stuck "pressed" by an unmasked counter would never turn another
    // page, so mask the churning bytes and re-baseline instead of staying wedged.
    if (device->activeSinceMs != 0 && (nowMs - device->activeSinceMs) > STUCK_ACTIVE_MS &&
        device->activeChangeCount >= VOLATILE_CHANGE_THRESHOLD) {
      device->volatileMask |= device->churnMask;
      memcpy(device->idleFrame, frame, HID_FRAME_BYTES);
      device->active = false;
      device->activeSinceMs = 0;
      device->churnMask = 0;
      device->activeChangeCount = 0;
      LOG_INF("BT", "%s active >%lu ms with churn, re-masked (volatileMask=0x%02X)", device->address.c_str(),
              STUCK_ACTIVE_MS, device->volatileMask);
    }
    return false;
  }

  // Rising edge: a button just went down.
  device->active = true;
  device->activeSinceMs = nowMs;
  device->churnMask = 0;
  device->activeChangeCount = 0;
  memcpy(device->prevFrame, frame, HID_FRAME_BYTES);

  // Remotes commonly expose the same press on several report characteristics,
  // which arrive within a few ms of each other. Collapse them into one turn.
  if (device->lastPressMs != 0 && (nowMs - device->lastPressMs) < MIN_PRESS_INTERVAL_MS) {
    return false;
  }
  device->lastPressMs = nowMs;
  return true;
}

// Static callback for HID notifications
void BluetoothHIDManager::onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (!g_instance || !pData || length == 0) return;

  ConnectedDevice* device = nullptr;
  if (pChar && pChar->getRemoteService()) {
    auto client = pChar->getRemoteService()->getClient();
    if (client) {
      device = g_instance->findConnectedDevice(client->getPeerAddress().toString());
    }
  }
  if (!device) return;

  const unsigned long nowMs = millis();
  device->lastActivityTime = nowMs;  // Keeps the connection (and sleep timer) alive

  if (g_instance->_debugCaptureEnabled) {
    char rawBuf[128] = {0};
    size_t offset = 0;
    const size_t dumpLen = length < HID_FRAME_BYTES ? length : HID_FRAME_BYTES;
    for (size_t i = 0; i < dumpLen && offset + 4 < sizeof(rawBuf); i++) {
      offset += snprintf(rawBuf + offset, sizeof(rawBuf) - offset, "%02X ", static_cast<unsigned>(pData[i]));
    }
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s mask=0x%02X", device->address.c_str(), static_cast<unsigned>(length),
            rawBuf, device->volatileMask);
  }

  if (!detectPress(device, pData, length, nowMs)) {
    return;
  }

  device->lastRemoteInputMs = nowMs;

  if (!g_instance->_buttonInjector) {
    return;
  }

  // Which physical button counts as "page forward" depends on the user's side
  // button layout, which this layer cannot see; the app supplies the resolver.
  const uint8_t button =
      g_instance->_pageTurnButtonProvider ? g_instance->_pageTurnButtonProvider() : HalGPIO::BTN_DOWN;

  // Injected as a pulse rather than a hold: clickers send press and release a
  // millisecond apart, so hold duration carries no usable information. HalGPIO
  // latches the press for at least one loop iteration (pendingVirtualPresses),
  // and a pulse can never leave a virtual button stuck down.
  g_instance->_buttonInjector(button, true);
  g_instance->_buttonInjector(button, false);
  LOG_INF("BT", ">>> REMOTE PRESS -> page forward (button %u) <<<", static_cast<unsigned>(button));
}

void BluetoothHIDManager::updateActivity() {
  if (_maintenanceSuspended) {
    return;
  }
  MaintenanceBusyScope busy(_maintenanceBusy);
  unsigned long now = millis();

  // No stale-release sweep is needed: presses are injected as pulses, so a
  // dropped release frame cannot leave a virtual button held down.

  // Connection maintenance every 10 seconds.
  if (now - lastMaintenanceCheck < 10000) {
    return;
  }
  lastMaintenanceCheck = now;

  // Check for one inactive connection and disconnect it in-place.
  std::string inactiveAddress;
  unsigned long inactiveTimeMs = 0;
  for (const auto& device : _connectedDevices) {
    if (device.lastActivityTime == 0) {
      continue;
    }

    unsigned long inactiveTime = now - device.lastActivityTime;
    if (inactiveTime > INACTIVITY_TIMEOUT_MS) {
      inactiveAddress = device.address;
      inactiveTimeMs = inactiveTime;
      break;
    }
  }

  if (!inactiveAddress.empty()) {
    LOG_INF("BT", "Device %s inactive for %lu ms, disconnecting", inactiveAddress.c_str(), inactiveTimeMs);
    disconnectFromDevice(inactiveAddress);
  }
}

void BluetoothHIDManager::checkAutoReconnect(bool userInputDetected) {
  if (!_enabled || _maintenanceSuspended) {
    return;
  }
  MaintenanceBusyScope busy(_maintenanceBusy);

  static unsigned long lastReconnectCheck = 0;
  static unsigned long lastReconnectAttempt = 0;
  unsigned long now = millis();

  // Advertisement-triggered reconnect: the background scan saw the bonded remote
  // advertising (= the user pressed one of ITS buttons). Connect right away —
  // these remotes only advertise for a short window after waking.
  if (_pendingBondedConnect) {
    _pendingBondedConnect = false;
    if (!_bondedDeviceAddress.empty() && now - lastReconnectAttempt >= 2000) {
      lastReconnectAttempt = now;
      LOG_INF("BT", "Bonded remote woke up, reconnecting to %s", _bondedDeviceAddress.c_str());
      if (connectToDevice(_bondedDeviceAddress)) {
        LOG_INF("BT", "Reconnected to bonded device %s", _bondedDeviceAddress.c_str());
        return;
      }
      LOG_ERR("BT", "Wake reconnect to %s failed: %s", _bondedDeviceAddress.c_str(), lastError.c_str());
    }
  }

  // Periodic bookkeeping every 5 seconds.
  if (now - lastReconnectCheck < 5000) {
    return;
  }
  lastReconnectCheck = now;

  // Remove stale disconnected clients from active list.
  for (auto it = _connectedDevices.begin(); it != _connectedDevices.end();) {
    if (!it->client || !it->client->isConnected()) {
      LOG_DBG("BT", "Pruning stale disconnected client entry: %s client=%p", it->address.c_str(), it->client);
      it = _connectedDevices.erase(it);
    } else {
      ++it;
    }
  }

  // Connected: nothing to reconnect and no need to listen for advertisements.
  if (!_connectedDevices.empty()) {
    stopBackgroundScan();
    return;
  }

  if (_bondedDeviceAddress.empty()) {
    return;
  }

  // Fast path: a physical button press on the device also tries a reconnect
  // (covers remotes that stay connectable without advertising).
  if (userInputDetected && now - lastReconnectAttempt >= 2000) {
    lastReconnectAttempt = now;
    LOG_INF("BT", "Button activity detected while disconnected, reconnecting to bonded device %s",
            _bondedDeviceAddress.c_str());
    if (connectToDevice(_bondedDeviceAddress)) {
      LOG_INF("BT", "Reconnected to bonded device %s", _bondedDeviceAddress.c_str());
      return;
    }
    LOG_ERR("BT", "Reconnect to bonded device %s failed: %s", _bondedDeviceAddress.c_str(), lastError.c_str());
  }

  // Disconnected with a bonded remote: keep listening for it to wake up.
  startBackgroundScan();
}

void BluetoothHIDManager::saveState() {
  LOG_DBG("BT", "Saving state (stub)");
  // Stub: would save paired devices to file
}

void BluetoothHIDManager::loadState() {
  LOG_DBG("BT", "Loading state (stub)");
  // Stub: would load paired devices from file
}
