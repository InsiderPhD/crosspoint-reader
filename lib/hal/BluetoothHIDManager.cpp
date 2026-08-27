#include "BluetoothHIDManager.h"

#include <DevicePolicy.h>
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

// Cap on the per-scan "skipped advertiser" diagnostic (see onScanResult).
static constexpr uint8_t SKIPPED_ADV_LOG_LIMIT = 24;

namespace {
// BLE intervals are in 1.25ms units and timeout is in 10ms units.
constexpr uint16_t BLE_CONN_MIN_INTERVAL = 12;  // 15ms
constexpr uint16_t BLE_CONN_MAX_INTERVAL = 24;  // 30ms
// Two latency regimes:
// - SETUP (0): connect + GATT discovery need a round trip per exchange, and any
//   slave latency multiplies each one — latency 33 during setup stretched a
//   reconnect into a ~24s blocked loop pass ("New max loop duration: 24429 ms")
//   and let the first attempt die mid-discovery. Setup runs snappy at 0.
// - IDLE (after subscribe): latency lets the clicker skip events and doze WITHIN
//   the link instead of deep-sleeping out of it. Forcing 0 here made the remote
//   give up ~2-3s after connect (silent -> 6s supervision -> reason 520 -> a
//   spent wake-press per reconnect). The AB Shutter3 clone itself requests
//   itvl 48-64 / latency 9 / timeout 500, and peer requests are accepted
//   (onConnParamsUpdateRequest) — this push covers remotes that never ask.
//   A pending press still transmits at the next event; input latency unchanged.
// Spec bound: timeout > 2 x (1+latency) x maxInterval = 2s < 6s. OK.
constexpr uint16_t BLE_CONN_SETUP_LATENCY = 0;
constexpr uint16_t BLE_CONN_IDLE_LATENCY = 33;
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
// Nothing here decodes keycodes. The detector answers "did a button just go
// down?" structurally; which button it was is a separate signature match.
constexpr unsigned long BASELINE_LEARN_MS = 1000;     // Post-connect window to learn the idle report
constexpr uint8_t VOLATILE_CHANGE_THRESHOLD = 3;      // Byte changes in a window => free-running, mask it
constexpr unsigned long MIN_PRESS_INTERVAL_MS = 120;  // Floor between injected page turns
constexpr unsigned long STUCK_ACTIVE_MS = 2500;       // Active this long with churning bytes => re-learn
constexpr unsigned long REST_GAP_MS = 300;            // Silence this long => the frame before it was the rest state

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
  // On an ACTIVE scan NimBLE withholds onResult() for a scannable legacy
  // advertiser until its SCAN_RSP arrives (NimBLEScan.cpp: the
  // `!isScannable()` branch). A remote that advertises ADV_IND but never
  // answers a scan request is therefore invisible to onResult() alone — it
  // looks like "the remote isn't advertising" when it is. Take the first
  // advertisement too; onScanResult() de-duplicates by address and back-fills
  // the name if a scan response does turn up later.
  void onDiscovered(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (g_instance) {
      g_instance->onScanResult(const_cast<NimBLEAdvertisedDevice*>(advertisedDevice));
    }
  }

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

  // Accept (default) the remote's own preferred parameters, but log them: what a
  // clicker asks for here is the ground truth for tuning BLE_CONN_* above.
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
    LOG_INF("BT", "Peer param request: itvl %u-%u (x1.25ms) latency %u timeout %u (x10ms)", params->itvl_min,
            params->itvl_max, params->latency, params->supervision_timeout);
    return true;
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

#if CROSSPOINT_BLE_EXCLUSIVE
  // CRITICAL: Disable WiFi when enabling Bluetooth
  // ESP32-C3 cannot have both WiFi and BLE enabled simultaneously
  if (WiFi.getMode() != WIFI_OFF) {
    LOG_INF("BT", "Disabling WiFi to enable Bluetooth (mutual exclusion)");
    WiFi.disconnect(true);  // true = turn off WiFi radio
    WiFi.mode(WIFI_OFF);
    delay(100);  // Brief delay to ensure WiFi is fully powered down
  }
#else
  // Roomy board with software coexistence compiled in
  // (CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y): leave WiFi up. The controller has the
  // heap it needs and the radio is time-shared, so a sync no longer costs the
  // user their remote connection. See DevicePolicy.h.
#endif

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

  // The remembered address may be a rotating private one; correct it from the
  // bond store before anything tries to use it.
  reconcileBondedAddressWithStore();

  // A bonded remote's address and address type are persisted in settings, so the
  // link can be established without ever seeing an advertisement. Arm a one-shot
  // direct connect rather than waiting for the background scan: a remote that
  // advertises in short bursts, or that never answers a SCAN_REQ, can be missed
  // by scanning indefinitely, while a connect procedure keeps the initiator
  // listening for its whole timeout and takes the link the moment one appears.
  // ...unless that address is a Resolvable Private Address AND we have a name to
  // recognise the remote by: an RPA rotates on the peer's own timer, so the
  // connect is a guaranteed BLE_HS_ETIMEOUT that blocks the loop task for the
  // full timeout. Go straight to the rediscovery scan instead.
  const bool rediscoverable = rememberedAddressIsRotating();
  if (rediscoverable) {
    LOG_INF("BT", "Remembered address %s is an RPA - waiting to rediscover '%s' by name instead",
            _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
  }
  _pendingEnableConnect = !_bondedDeviceAddress.empty() && !rediscoverable;

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
  _pendingEnableConnect = false;
  _pendingAddrAdopt = false;
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
  _skippedAdvLogs = 0;
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
  // Active, not passive: a remote using a rotating private address can only be
  // recognised by its NAME, and a name usually travels in the scan response,
  // which passive scanning never asks for. The extra SCAN_REQ traffic is bounded
  // to the window where the remote is disconnected.
  pScan->setActiveScan(true);
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
    bool isBondedRemote = !_bondedDeviceAddress.empty() && address == _bondedDeviceAddress;
    // A remote that advertises under a Resolvable Private Address rotates it on
    // its own timer, so the remembered address stops matching and it becomes
    // invisible to an address compare. Fall back to what does not rotate: the
    // name captured when it was paired.
    const bool matchedByName =
        !isBondedRemote && !_bondedDeviceName.empty() && advertisedDevice->getName() == _bondedDeviceName;
    if (matchedByName) {
      LOG_INF("BT", "Bonded remote '%s' is advertising as %s (was %s) - address rotated", _bondedDeviceName.c_str(),
              address.c_str(), _bondedDeviceAddress.c_str());
      // Only stage it: this runs on the NimBLE host task, and persisting the new
      // address writes SPIFFS. The loop task adopts it in checkAutoReconnect().
      snprintf(_rediscoveredAddr, sizeof(_rediscoveredAddr), "%s", address.c_str());
      _pendingAddrAdopt = true;
      isBondedRemote = true;
    }
    if (isBondedRemote) {
      // The advertisement carries the authoritative address TYPE — refresh it so
      // the reconnect targets the peer correctly even for bonds saved before the
      // type was persisted (a random-address remote ignores PUBLIC-typed connects).
      _bondedAddrType = advertisedDevice->getAddress().getType();
      LOG_INF("BT", "Bonded remote is advertising (addr type %u), scheduling reconnect", _bondedAddrType);
      NimBLEScan* pScan = NimBLEDevice::getScan();
      if (pScan) {
        pScan->stop();
      }
      _backgroundScanActive = false;
      _pendingBondedConnect = true;
    }
    return;
  }

  // Only the pairing screen's foreground scan builds the device list. Without
  // this guard a trailing callback after stopScan(), or the second callback for
  // the advertisement that just satisfied the background reconnect above, would
  // quietly append to a list nobody is showing.
  if (!_scanning) return;

  std::string name = advertisedDevice->getName();
  int rssi = advertisedDevice->getRSSI();

  // Check if device advertises HID service. Some remotes omit the 0x1812 UUID
  // from the advertisement and only expose the appearance value, so accept the
  // HID appearance category (0x03Cx >> 6 == 0x0F: keyboard, mouse, gamepad...)
  // as equally good evidence — otherwise a nameless one is filtered out below.
  bool isHID = advertisedDevice->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));
  if (!isHID && advertisedDevice->haveAppearance() && (advertisedDevice->getAppearance() >> 6) == 0x0F) {
    isHID = true;
  }

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
  // Only a bounded sample is logged: busy RF means dozens of these per scan.
  if (name.empty() && !isHID) {
    // Logging the drop matters: when a remote that is demonstrably in pairing
    // mode never appears in the list, this is the only way to tell "the radio
    // never saw it" from "we filtered it out".
    if (_skippedAdvLogs < SKIPPED_ADV_LOG_LIMIT) {
      _skippedAdvLogs++;
      LOG_DBG("BT", "Skipped advertiser %s type=%u appearance=0x%04X RSSI:%d", address.c_str(),
              advertisedDevice->getAdvType(),
              advertisedDevice->haveAppearance() ? advertisedDevice->getAppearance() : 0, rssi);
    }
    return;
  }

  // Add new device
  BluetoothDevice device;
  device.address = address;
  device.name = name.empty() ? "Unknown" : name;
  device.rssi = rssi;
  device.isHID = isHID;
  device.addrType = advertisedDevice->getAddress().getType();

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

  // Connect + GATT discovery is the most timing-sensitive BLE procedure and can
  // run from the idle main loop with the CPU clocked down; force full speed for
  // its duration (same rationale as the disconnect path below).
  HalPowerManager::Lock powerLock;
  stopBackgroundScan();

  // A stray connect procedure can still be in flight here — NimBLE's
  // CONN_REATTEMPT re-initiates a failed connection internally, and a
  // timed-out attempt's cancel can lag. ble_gap_connect() then rejects our
  // user-initiated connect with BLE_HS_EALREADY (rc=2) in ~0.5s. Nothing else
  // connects from this task, so anything active now is orphaned: cancel it.
  if (ble_gap_conn_active()) {
    LOG_INF("BT", "Cancelling stale in-flight connect attempt");
    ble_gap_conn_cancel();
    // The cancel lands via a GAP event a few ms later; wait it out (bounded)
    // so the fresh connect below doesn't bounce off the same EALREADY.
    for (int i = 0; i < 50 && ble_gap_conn_active(); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Resolve the peer's address TYPE: a CONNECT_IND targeting a random static
  // address (top two bits set, e.g. ff:.. clickers) as BLE_ADDR_PUBLIC is
  // ignored by the peer — it keeps advertising and every connect times out.
  // Prefer the type captured from this session's advertisements (discovered
  // list), then the bonded record (settings at boot / background-scan refresh).
  uint8_t addrType = BLE_ADDR_PUBLIC;
  bool typeKnown = false;
  for (const auto& dev : _discoveredDevices) {
    if (dev.address == address) {
      addrType = dev.addrType;
      typeKnown = true;
      break;
    }
  }
  if (!typeKnown && address == _bondedDeviceAddress) {
    addrType = _bondedAddrType;
  }
  NimBLEAddress bleAddress(address, addrType);

  // First: adopt any client that already holds a LIVE link to this peer,
  // matching by address BYTES only. The remote chooses its advertised address
  // TYPE per session (both learned clickers lie about TxAdd), and the type is
  // part of NimBLE's lookup key — a type-mismatched lookup misses the owning
  // client, NimBLE's early "connection already exists" guard then fails every
  // connect instantly WITHOUT setting a new error code, and the stale rc from
  // the client's previous failure makes the log look like EALREADY forever.
  const auto liveClientForPeer = [&bleAddress]() -> NimBLEClient* {
    for (NimBLEClient* connected : NimBLEDevice::getConnectedClients()) {
      if (memcmp(connected->getPeerAddress().getBase()->val, bleAddress.getBase()->val, 6) == 0) {
        return connected;
      }
    }
    return nullptr;
  };

  NimBLEClient* pClient = liveClientForPeer();
  const bool adoptedLive = (pClient != nullptr);

  // A GAP link with NO live client (half-torn-down state) would hit the same
  // early guard with nothing to adopt — terminate it so the connect can run.
  if (!pClient) {
    for (uint8_t type = 0; type <= 1; type++) {
      ble_addr_t probe = *bleAddress.getBase();
      probe.type = type;
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find_by_addr(&probe, &desc) == 0) {
        LOG_INF("BT", "Terminating orphaned link to %s (handle %u)", address.c_str(), desc.conn_handle);
        ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 50 && ble_gap_conn_find_by_addr(&probe, &desc) == 0; i++) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        break;
      }
    }
  }

  // Reuse existing disconnected client objects to avoid NimBLE deleteClient() on this target.
  if (!pClient) {
    pClient = NimBLEDevice::getClientByPeerAddress(bleAddress);
  }
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
  pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_SETUP_LATENCY, BLE_CONN_TIMEOUT,
                               BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);

  if (!pClient->isConnected()) {
    pClient->deleteServices();
  }

  // Set connection callbacks
  static ClientCallbacks clientCallbacks;
  pClient->setClientCallbacks(&clientCallbacks, false);

  // connect() with one in-place recovery: BLE_HS_EALREADY (rc=2) means a
  // connect procedure is in flight even though the entry guard above saw none
  // — a timed-out attempt's ble_gap_conn_cancel() completes asynchronously and
  // can land between that check and this call. Cancel it and retry once.
  const auto attemptConnect = [](NimBLEClient* client, const NimBLEAddress& addr) {
    if (client->connect(addr)) return true;
    if (client->getLastError() != BLE_HS_EALREADY) return false;
    LOG_INF("BT", "Connect blocked by an in-flight procedure, cancelling it and retrying");
    ble_gap_conn_cancel();
    for (int i = 0; i < 50 && ble_gap_conn_active(); i++) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    return client->connect(addr);
  };

  // Connect to device — unless a live link already exists. A stray connect
  // procedure can complete in the background (observed with the Insta360
  // remote), leaving a GAP connection the manager's bookkeeping doesn't know
  // about; NimBLE then refuses any second connect to that peer (instant
  // failure, rc=0 from the early "connection already exists" guard). Adopt
  // the link and continue to service discovery instead.
  // A blocked procedure can COMPLETE INTO A CONNECTION in the ~ms between our
  // cancel and the retry (observed live: retry fails instantly on NimBLE's
  // "connection already exists" guard, which never sets an error code). The
  // link is up and usable — find whichever client owns it and take it.
  const auto adoptRacedLink = [&](NimBLEClient*& client) {
    NimBLEClient* survivor = liveClientForPeer();
    if (!survivor) {
      return false;
    }
    LOG_INF("BT", "Connect attempt lost a race but the link is up — adopting it");
    client = survivor;
    client->setSelfDelete(false, false);
    client->setClientCallbacks(&clientCallbacks, false);
    return true;
  };

  if (pClient->isConnected()) {
    LOG_INF("BT", "Adopting existing live connection to %s", address.c_str());
  } else if (!attemptConnect(pClient, bleAddress) && !adoptRacedLink(pClient)) {
    // A reused client can carry stale state, so it gets one retry with a fresh
    // client. A client that was already fresh gets no second attempt: connect()
    // blocks the loop task for BLE_CONNECT_TIMEOUT_MS per call, so retrying the
    // same client only doubles the freeze for the same outcome.
    if (!hadExistingClient) {
      lastError = "Connection failed";
      lastStatus = BtStatus::ConnectFailed;
      // The host error code tells an instant rejection (peer dropped us: bond or
      // whitelist mismatch) apart from a timeout (peer not advertising/in range).
      // Caveat: after an INSTANT failure the rc can be stale — NimBLE's early
      // guards return false without refreshing it.
      LOG_ERR("BT", "Failed to connect to %s (rc=%d)", address.c_str(), pClient->getLastError());
      return false;
    }
    LOG_INF("BT", "Reconnect with existing client failed for %s, retrying with fresh client", address.c_str());
    NimBLEClient* freshClient = NimBLEDevice::createClient(bleAddress);
    if (!freshClient) {
      lastError = "Failed to create BLE client";
      lastStatus = BtStatus::ClientFailed;
      LOG_ERR("BT", "Failed to create fresh BLE client for %s", address.c_str());
      return false;
    }
    pClient = freshClient;
    pClient->setSelfDelete(false, false);
    pClient->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS);
    pClient->setConnectionParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_SETUP_LATENCY, BLE_CONN_TIMEOUT,
                                 BLE_CONN_SCAN_INTERVAL, BLE_CONN_SCAN_WINDOW);
    pClient->setClientCallbacks(&clientCallbacks, false);

    if (!attemptConnect(pClient, bleAddress) && !adoptRacedLink(pClient)) {
      lastError = "Connection failed";
      lastStatus = BtStatus::ConnectFailed;
      LOG_ERR("BT", "Failed to connect to %s (rc=%d)", address.c_str(), pClient->getLastError());
      return false;
    }
  }

  const bool connParamsUpdated =
      pClient->updateConnParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_SETUP_LATENCY, BLE_CONN_TIMEOUT);
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

  // Setup is done — relax into the doze-friendly idle regime so the clicker can
  // skip connection events instead of deep-sleeping out of the link. If the
  // remote later requests its own parameters, that request wins (accepted in
  // onConnParamsUpdateRequest).
  const bool idleParamsUpdated =
      pClient->updateConnParams(BLE_CONN_MIN_INTERVAL, BLE_CONN_MAX_INTERVAL, BLE_CONN_IDLE_LATENCY, BLE_CONN_TIMEOUT);
  LOG_INF("BT", "Idle conn params request (latency %u): %d", BLE_CONN_IDLE_LATENCY, idleParamsUpdated);

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

#if !CROSSPOINT_BLE_EXCLUSIVE
  // Roomy board: the section build / sync / render that asked for this pause has
  // the heap it needs with the stack up, so keep the remote connected instead of
  // making the user press a button to get it back. Returning false leaves
  // BleMemoryPause's dtor a no-op too. See DevicePolicy.h.
  LOG_DBG("BT", "Memory pause not needed on this board; BLE stays up (free %u, largest %u)", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  return false;
#endif

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
  // Explicit hold-off (set by the reader's degraded-heap render guard): without
  // it, guard-pause -> ~5s retry -> enable -> guard-pause ping-pongs forever,
  // reloading the epub and re-rendering every cycle.
  if (_restoreDeferForMs != 0 && now - _restoreDeferStampMs < _restoreDeferForMs) {
    return false;
  }
#if CROSSPOINT_BLE_EXCLUSIVE
  // WiFi settle: refuse to bring the BT controller up until the chip has had
  // quiet time since the last WiFi teardown (see BLE_WIFI_SETTLE_MS). The
  // WiFi->BT controller handoff freeze is a C3 hazard; a coexisting board never
  // takes WiFi down for BLE in the first place, so there is nothing to wait for.
  if (_lastWifiActivityMs != 0 && now - _lastWifiActivityMs < BLE_WIFI_SETTLE_MS) {
    return false;
  }
#endif
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

void BluetoothHIDManager::deferAutoRestore(unsigned long forMs) {
  _restoreDeferStampMs = millis();
  _restoreDeferForMs = forMs;
  LOG_INF("BT", "Auto-restore deferred for %lums", forMs);
}

void BluetoothHIDManager::setBondedDevice(const std::string& address, const std::string& name, uint8_t addrType) {
  _bondedDeviceAddress = address;
  _bondedAddrType = addrType;
  _bondedDeviceName = name;
  LOG_INF("BT", "Bonded device set: %s (%s)", _bondedDeviceAddress.c_str(), _bondedDeviceName.c_str());
}

// True when the remembered address is a Resolvable Private Address that the peer
// rotates on its own timer AND we kept a name we can rediscover it by. Connecting
// to such an address is a guaranteed timeout, and connectToDevice() blocks the
// loop task for the whole of it — so every caller should wait for the rediscovery
// scan instead.
bool BluetoothHIDManager::rememberedAddressIsRotating() const {
  if (_bondedDeviceAddress.empty() || _bondedDeviceName.empty()) {
    return false;
  }
  return NimBLEAddress(_bondedDeviceAddress, _bondedAddrType).isRpa();
}

void BluetoothHIDManager::setBondedAddressUpdatedCallback(void (*callback)(const char*, uint8_t)) {
  _bondedAddrUpdatedCallback = callback;
}

// A BLE peripheral is free to advertise under a Resolvable Private Address that
// rotates on its own timer (CONFIG_BT_NIMBLE_RPA_TIMEOUT, 900s by default). The
// pairing screen stores whatever address the scan reported, so for such a remote
// the remembered address is dead within minutes: a direct connect times out
// (rc=13) and the background scan's address compare never matches, which reads
// as "paired but it will never reconnect".
//
// The durable identity is the bond's identity address, which the peer hands over
// as part of the ID key exchange (LL privacy is compiled in, so NimBLE adds
// BLE_SM_PAIR_KEY_DIST_ID to both key distributions — NimBLEDevice.cpp:994-997).
// ble_store_util_bonded_peers() returns exactly those identity addresses; the
// controller resolves the rotating RPA back to it via the resolving list.
void BluetoothHIDManager::reconcileBondedAddressWithStore() {
  if (_bondedDeviceAddress.empty()) {
    return;
  }
  const int numBonds = NimBLEDevice::getNumBonds();
  const NimBLEAddress remembered(_bondedDeviceAddress, _bondedAddrType);
  LOG_INF("BT", "Bond store holds %d bond(s); remembered %s (type %u%s)", numBonds, _bondedDeviceAddress.c_str(),
          _bondedAddrType, remembered.isRpa() ? ", RPA" : "");

  if (numBonds <= 0) {
    // Paired per settings but no key material: the bond was lost (NVS erase, or
    // the remote was re-paired to something else). Nothing here can reconnect it.
    LOG_ERR("BT", "No bond stored for %s - the remote has to be paired again", _bondedDeviceAddress.c_str());
    return;
  }
  // Only auto-adopt when there is exactly one bond. With several, index 0 is not
  // necessarily this remote and guessing would point us at the wrong peer.
  if (numBonds != 1) {
    return;
  }

  const NimBLEAddress identity = NimBLEDevice::getBondedAddress(0);
  if (identity.isNull()) {
    return;
  }
  const std::string identityStr = identity.toString();
  if (identityStr == _bondedDeviceAddress) {
    return;
  }

  // Report only. Adopting this was tried and REVERTED: on the user's eMote the
  // store handed back an address that timed out exactly like the stale RPA it
  // replaced, and overwriting the remembered address destroyed the one link we
  // do trust — the bonded NAME, which the rediscovery in onScanResult uses.
  // Connecting to an identity address also needs the controller to resolve RPAs
  // against the resolving list (peer type BLE_ADDR_*_ID), which is not wired up
  // here; until that is proven on hardware this stays a diagnostic.
  LOG_INF("BT", "Bond identity address is %s (type %u); remembered %s - NOT adopting", identityStr.c_str(),
          identity.getType(), _bondedDeviceAddress.c_str());
}

void BluetoothHIDManager::setButtonMapping(const uint8_t backIndex, const uint8_t backValue, const uint8_t fwdIndex,
                                           const uint8_t fwdValue) {
  _backSigIndex = backIndex;
  _backSigValue = backValue;
  _fwdSigIndex = fwdIndex;
  _fwdSigValue = fwdValue;
  _mappingLooksStale = false;
  if (backIndex != 0xFF) {
    LOG_INF("BT", "Button mapping: back sig %u=0x%02X, fwd sig %u=0x%02X", static_cast<unsigned>(backIndex),
            static_cast<unsigned>(backValue), static_cast<unsigned>(fwdIndex), static_cast<unsigned>(fwdValue));
  } else {
    LOG_INF("BT", "Button mapping cleared: every press pages forward");
  }
}

bool BluetoothHIDManager::hasRecentActivity() const {
  // A remote press in the last 2 minutes holds off auto-sleep, so the device
  // stays awake between the widely-spaced page turns of remote-driven reading.
  //
  // Deliberately keyed on lastRemoteInputMs (actual detected presses), NOT
  // lastActivityTime: the latter is seeded at connect time, and a constantly-
  // advertising remote re-connects right after every 5-minute inactivity
  // disconnect — counting the connection itself as activity made that cycle
  // reset the sleep timer forever, and the device never slept.
  unsigned long now = millis();
  for (const auto& device : _connectedDevices) {
    if (device.lastRemoteInputMs > 0) {
      unsigned long timeSinceInput = now - device.lastRemoteInputMs;
      if (timeSinceInput < 120000) {  // 2 minute threshold
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
// button has a distinct code at all vary per model. So nothing is decoded
// semantically. The detector finds the moment a button goes down structurally,
// and identifies WHICH button only by its signature — the (byte, value) that
// left idle — matched against what the mapping wizard learned:
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
PressDetectorState* BluetoothHIDManager::detectorFor(ConnectedDevice* device, const size_t length,
                                                     const bool allocate) {
  // Shapes are keyed by report length: mixing shapes in one learning state lets
  // them mask each other's signal bytes (see PressDetectorState in the header).
  const uint8_t lenKey = static_cast<uint8_t>(length > 0xFF ? 0xFF : length);
  for (auto& det : device->detectors) {
    if (det.reportLen == lenKey) {
      return &det;
    }
  }
  if (!allocate) {
    return nullptr;
  }
  for (auto& det : device->detectors) {
    if (det.reportLen == 0) {
      det.reportLen = lenKey;
      return &det;
    }
  }
  // More shapes than slots: ignore the extras rather than corrupting a learned
  // one. Raise MAX_REPORT_SHAPES if a real remote ever hits this.
  return nullptr;
}

bool BluetoothHIDManager::detectPress(ConnectedDevice* device, const uint8_t* data, const size_t length,
                                      const unsigned long nowMs) {
  PressDetectorState* det = detectorFor(device, length, true);
  if (!det) {
    return false;
  }

  const size_t n = length < HID_FRAME_BYTES ? length : HID_FRAME_BYTES;
  uint8_t frame[HID_FRAME_BYTES] = {0};
  memcpy(frame, data, n);

  // Phase 1: learn this shape's idle report and its free-running bytes.
  if (!det->baselineReady) {
    if (det->baselineFrames == 0) {
      // First frame of this shape: it opens the window and seeds the reference.
      det->baselineStartMs = nowMs;
      det->baselineFrames = 1;
      det->lastFrameMs = nowMs;
      memcpy(det->idleFrame, frame, HID_FRAME_BYTES);
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        if (frame[i] == 0 && det->byteZeroCount[i] < 0xFF) det->byteZeroCount[i]++;
      }
      return false;
    }

    if ((nowMs - det->baselineStartMs) < BASELINE_LEARN_MS) {
      // Silence since the previous frame means the report SAT at that frame:
      // it is a release, not a press. Remember it as the rest candidate.
      if ((nowMs - det->lastFrameMs) >= REST_GAP_MS) {
        memcpy(det->restFrame, det->idleFrame, HID_FRAME_BYTES);
        det->restFrameValid = true;
      }
      det->lastFrameMs = nowMs;
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        if (frame[i] != det->idleFrame[i] && det->byteChangeCount[i] < 0xFF) {
          det->byteChangeCount[i]++;
        }
        // Count how often each byte sits at 0x00: a keycode RESTS at zero
        // between presses, an axis/counter merely transits it (see below).
        if (frame[i] == 0 && det->byteZeroCount[i] < 0xFF) det->byteZeroCount[i]++;
      }
      memcpy(det->idleFrame, frame, HID_FRAME_BYTES);
      if (det->baselineFrames < 0xFFFF) {
        det->baselineFrames++;
      }
      return false;
    }

    // Window closed. Note this frame is deliberately NOT folded into the
    // reference: on a remote that only transmits on press, it IS the press, and
    // adopting it as "idle" would invert the detector for the whole session.
    if (det->baselineFrames < 2) {
      // Silent shape: no idle traffic to characterise, so assume all-zero idle.
      memset(det->idleFrame, 0, HID_FRAME_BYTES);
    } else {
      // The window closes on whatever frame happens to arrive after 1000 ms —
      // which is a PRESSED frame if the user is holding a button as it expires
      // (easy to do in the mapping wizard, which asks for presses immediately
      // after connecting). A frame observed to have been followed by silence is
      // strictly better evidence of the rest state, so prefer it.
      if (det->restFrameValid) {
        memcpy(det->idleFrame, det->restFrame, HID_FRAME_BYTES);
      }
      for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
        // A byte RESTING at 0x00 for a meaningful share of the window is the
        // shape's signal byte (a keycode that rests at 0 between presses), NOT
        // a free-running counter. Its true idle is 0 — even if the window
        // happened to close on a press frame — and it must never be masked or
        // the detector goes blind. This is what keeps a press-only clicker
        // (AB Shutter3: byte0 = E9 pressed / 00 released) working even when
        // the user presses during the learn window. The frequency test is what
        // separates it from the high byte of a 16-bit joystick axis, which
        // VISITS zero for a frame or two when the axis transits below 0x100:
        // adopting idle 0 for that byte inverts the detector for the session
        // and makes press signatures differ between sessions.
        const bool restsAtZero =
            det->byteZeroCount[i] > 0 &&
            (det->byteZeroCount[i] == 0xFF || static_cast<uint16_t>(det->byteZeroCount[i]) * 4 >= det->baselineFrames);
        if (restsAtZero) {
          det->idleFrame[i] = 0;
        } else if (det->byteChangeCount[i] >= VOLATILE_CHANGE_THRESHOLD) {
          // Rarely-or-never zero AND churned: a real rolling counter/axis.
          det->volatileMask |= static_cast<uint8_t>(1u << i);
        }
      }
    }
    memcpy(det->prevFrame, det->idleFrame, HID_FRAME_BYTES);
    det->baselineReady = true;
    LOG_INF("BT", "Idle report learned for %s len=%u: frames=%u volatileMask=0x%02X", device->address.c_str(),
            static_cast<unsigned>(det->reportLen), static_cast<unsigned>(det->baselineFrames), det->volatileMask);

    // A stored back signature whose value IS this shape's idle value can never
    // match a press (the deviation guard in the decode rejects it), so the
    // remote silently pages forward on every button. That mapping was captured
    // in a session whose idle baseline was wrong, and the only cure is a re-run
    // of the wizard — say so instead of failing mutely.
    const uint8_t mapBackIdx = g_instance->_backSigIndex;
    if (mapBackIdx < HID_FRAME_BYTES) {
      // Every learned shape must be unable to match before we say so: the back
      // button may well live on a shape that has not connected/learned yet, and
      // a false "re-map me" on a working remote is worse than a missing one.
      bool anyShapeCanMatch = false;
      for (const auto& other : device->detectors) {
        if (other.reportLen == 0 || !other.baselineReady) {
          continue;
        }
        if ((other.volatileMask & static_cast<uint8_t>(1u << mapBackIdx)) != 0 ||
            g_instance->_backSigValue != other.idleFrame[mapBackIdx]) {
          anyShapeCanMatch = true;
          break;
        }
      }
      if (!anyShapeCanMatch) {
        LOG_ERR("BT", "Stale mapping: back sig %u=0x%02X is the IDLE value of every learned shape - re-run BT setup",
                static_cast<unsigned>(mapBackIdx), static_cast<unsigned>(g_instance->_backSigValue));
      }
      g_instance->_mappingLooksStale = !anyShapeCanMatch;
    }
    // Fall through: evaluate this frame normally so a press that arrives right
    // as the window closes still turns a page.
  }

  // Phase 2: does this frame differ from idle on any byte we still trust?
  bool active = false;
  for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
    if ((det->volatileMask & static_cast<uint8_t>(1u << i)) != 0) {
      continue;
    }
    if (frame[i] != det->idleFrame[i]) {
      active = true;
      break;
    }
  }

  if (!active) {
    det->active = false;
    det->activeSinceMs = 0;
    det->churnMask = 0;
    det->activeChangeCount = 0;
    memcpy(det->prevFrame, frame, HID_FRAME_BYTES);
    return false;
  }

  if (det->active) {
    // Already down. Either a real hold, or a counter byte we failed to mask
    // during learning. They are told apart by whether the bytes keep changing:
    // a held button's report is constant, a counter's is not.
    for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
      if (frame[i] != det->prevFrame[i]) {
        det->churnMask |= static_cast<uint8_t>(1u << i);
        if (det->activeChangeCount < 0xFF) {
          det->activeChangeCount++;
        }
        break;
      }
    }
    memcpy(det->prevFrame, frame, HID_FRAME_BYTES);

    // A shape stuck "pressed" by an unmasked counter would never turn another
    // page, so mask the churning bytes and re-baseline instead of staying wedged.
    if (det->activeSinceMs != 0 && (nowMs - det->activeSinceMs) > STUCK_ACTIVE_MS &&
        det->activeChangeCount >= VOLATILE_CHANGE_THRESHOLD) {
      det->volatileMask |= det->churnMask;
      memcpy(det->idleFrame, frame, HID_FRAME_BYTES);
      det->active = false;
      det->activeSinceMs = 0;
      det->churnMask = 0;
      det->activeChangeCount = 0;
      LOG_INF("BT", "%s len=%u active >%lu ms with churn, re-masked (volatileMask=0x%02X)", device->address.c_str(),
              static_cast<unsigned>(det->reportLen), STUCK_ACTIVE_MS, det->volatileMask);
    }
    return false;
  }

  // Rising edge: a button just went down. Its signature — which unmasked byte
  // left idle, and to what value — is what tells a two-button remote's buttons
  // apart, so record it alongside the edge.
  det->active = true;
  det->activeSinceMs = nowMs;
  det->churnMask = 0;
  det->activeChangeCount = 0;
  for (size_t i = 0; i < HID_FRAME_BYTES; i++) {
    if ((det->volatileMask & static_cast<uint8_t>(1u << i)) != 0) {
      continue;
    }
    if (frame[i] != det->idleFrame[i]) {
      device->lastPressSigIndex = static_cast<uint8_t>(i);
      device->lastPressSigValue = frame[i];
      break;
    }
  }

  // WHICH unmasked byte fires the edge first depends on this session's learned
  // mask — and the mask depends on what traffic happened to flow during the
  // learn window, so it can differ from the wizard's session. The edge frame's
  // CONTENT is stable across sessions, so the back decision matches the learned
  // signature against the frame, not against the first-deviating byte: back iff
  // the frame holds the back value at the back index AND that byte actually
  // left idle there (or is masked this session, in which case its idle is
  // meaningless) — the deviation guard stops a stored value from coinciding
  // with another shape's resting byte or zero padding.
  // g_instance is non-null here: detectPress is only reached via onHIDNotify,
  // which returns early without it.
  const uint8_t backIdx = g_instance->_backSigIndex;
  device->lastPressIsBack =
      backIdx < HID_FRAME_BYTES && frame[backIdx] == g_instance->_backSigValue &&
      (frame[backIdx] != det->idleFrame[backIdx] || (det->volatileMask & static_cast<uint8_t>(1u << backIdx)) != 0);

  memcpy(det->prevFrame, frame, HID_FRAME_BYTES);

  // Remotes commonly expose the same press on several report characteristics,
  // which arrive within a few ms of each other — even with different shapes.
  // Collapse them into one turn (device-level on purpose).
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
    const PressDetectorState* det = detectorFor(device, length, false);
    LOG_INF("BTDBG", "addr=%s len=%u raw=%s mask=0x%02X", device->address.c_str(), static_cast<unsigned>(length),
            rawBuf, det ? det->volatileMask : 0);
  }

  if (!detectPress(device, pData, length, nowMs)) {
    return;
  }

  // Publish the press signature BEFORE the input stamp: the mapping wizard in
  // the loop task watches lastRemoteInputMs() for a change and then reads the
  // signature, so this order guarantees it never pairs a new stamp with a stale
  // signature.
  g_instance->_lastPressSigIndex = device->lastPressSigIndex;
  g_instance->_lastPressSigValue = device->lastPressSigValue;
  device->lastRemoteInputMs = nowMs;

  // The mapping wizard captures presses without letting them drive the menu it
  // is running in.
  if (g_instance->_injectionSuppressed) {
    LOG_INF("BT", "Remote press captured (sig %u=0x%02X), injection suppressed",
            static_cast<unsigned>(device->lastPressSigIndex), static_cast<unsigned>(device->lastPressSigValue));
    return;
  }

  if (!g_instance->_buttonInjector) {
    return;
  }

  // A press whose edge frame matched the learned back signature (see
  // detectPress) pages back; everything else — unmapped remotes, unlearned
  // extra buttons — pages forward, the behaviour a fresh remote gets with no
  // setup.
  const bool pageForward = !device->lastPressIsBack;

  // Which physical button counts as page forward/back depends on the user's
  // side button layout, which this layer cannot see; the app supplies the resolver.
  const uint8_t button = g_instance->_pageTurnButtonProvider ? g_instance->_pageTurnButtonProvider(pageForward)
                                                             : (pageForward ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);

  // Injected as a pulse rather than a hold: clickers send press and release a
  // millisecond apart, so hold duration carries no usable information. HalGPIO
  // latches the press for at least one loop iteration (pendingVirtualPresses),
  // and a pulse can never leave a virtual button stuck down.
  g_instance->_buttonInjector(button, true);
  g_instance->_buttonInjector(button, false);
  LOG_INF("BT", ">>> REMOTE PRESS -> page %s (button %u) <<<", pageForward ? "forward" : "back",
          static_cast<unsigned>(button));
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
  // Adopt an address the background scan rediscovered by name. Done here, on the
  // loop task, because persisting it writes SPIFFS.
  if (_pendingAddrAdopt) {
    _pendingAddrAdopt = false;
    if (_rediscoveredAddr[0] != '\0' && _bondedDeviceAddress != _rediscoveredAddr) {
      _bondedDeviceAddress = _rediscoveredAddr;
      LOG_INF("BT", "Bonded remote address updated to %s (type %u)", _bondedDeviceAddress.c_str(), _bondedAddrType);
      if (_bondedAddrUpdatedCallback) {
        _bondedAddrUpdatedCallback(_bondedDeviceAddress.c_str(), _bondedAddrType);
      }
    }
  }

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

  // Bluetooth just came back up with a remote already bonded: go and get it,
  // rather than waiting for it to advertise. Reader-only for the same reason as
  // the physical-press fast path below — connectToDevice() blocks the loop task
  // for the full connect timeout, and on the Bluetooth settings screen that
  // freezes the pairing UI (the guided setup enables then scans, and the
  // "Reconnect remote" row already connects on demand). The flag is consumed
  // either way so it can't fire later at a surprising moment.
  if (_pendingEnableConnect) {
    _pendingEnableConnect = false;
    const bool inReaderNow = _readerContextCallback && _readerContextCallback();
    // Deliberately not re-checking rememberedAddressIsRotating() here: the flag
    // is only armed when it was already false at enable() time.
    if (inReaderNow && !_scanning && _connectedDevices.empty() && !_bondedDeviceAddress.empty()) {
      lastReconnectAttempt = now;
      LOG_INF("BT", "Bluetooth back up, forcing a connect to bonded remote %s", _bondedDeviceAddress.c_str());
      if (connectToDevice(_bondedDeviceAddress)) {
        LOG_INF("BT", "Connected to bonded device %s", _bondedDeviceAddress.c_str());
        return;
      }
      // Not an error worth surfacing: the remote is simply asleep or out of
      // range. The background scan below picks it up when it wakes.
      LOG_INF("BT", "Forced connect to %s did not land (%s); falling back to the wake-up scan",
              _bondedDeviceAddress.c_str(), lastError.c_str());
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
  // (covers remotes that stay connectable without advertising). Reader only:
  // connectToDevice() blocks the loop task for the full connect timeout, and in
  // the Bluetooth settings screen every press is the user navigating — a
  // blocking reconnect to the old bonded remote freezes the pairing UI. There
  // the background scan (below) still reconnects a remote that wakes up, and
  // the device list connects explicitly.
  const bool inReader = _readerContextCallback && _readerContextCallback();
  if (userInputDetected && inReader && !rememberedAddressIsRotating() && now - lastReconnectAttempt >= 2000) {
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
