#pragma once

#include <Arduino.h>

#include <functional>
#include <string>
#include <vector>

// Forward declarations
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

// Outcome of the last lifecycle/connection call, for the UI to render.
//
// A code rather than a string: this layer sits below I18n and must not depend on
// it, but every one of these ends up in front of the user. `lastError` is kept
// alongside purely as English detail for the serial log.
enum class BtStatus : uint8_t {
  None = 0,
  Connected,
  NotEnoughMemory,
  HeapFragmented,
  StartFailed,
  ScanFailed,
  NotEnabled,
  ClientFailed,
  ConnectFailed,
  NoHidService,
  NoReportChar,
  SubscribeFailed,
};

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi;
  bool isHID = false;
  uint8_t addrType = 0;  // BLE_ADDR_PUBLIC/BLE_ADDR_RANDOM, captured from the advertisement
};

// Number of leading HID report bytes the press detector considers. Every clicker
// we have seen encodes its button state well inside this window, and the fixed
// size keeps ConnectedDevice free of heap allocations.
inline constexpr size_t HID_FRAME_BYTES = 8;

// Distinct report SHAPES (by report length) the detector tracks per device.
// Multi-function remotes emit several: the Insta360 GPS remote sends a 3-byte
// button report AND a 6-byte analog/dial report. Sharing one idle/mask between
// them is what broke it: interleaved shapes make each other's constant bytes
// look churny, the churn re-masker then masks the real signal byte, and the
// detector goes blind. Fixed pool, ~48 bytes per slot — no heap.
inline constexpr size_t MAX_REPORT_SHAPES = 3;

// Per-report-shape press detector state. We never decode HID keycodes. Instead
// we learn what this shape's idle report looks like, mask off the bytes that
// free-run (rolling counters, joystick axes), and treat every idle -> active
// transition of the remaining bytes as one press.
struct PressDetectorState {
  uint8_t reportLen = 0;                     // Report length this slot tracks; 0 = slot free
  uint8_t idleFrame[HID_FRAME_BYTES] = {0};  // Reference "nothing pressed" report
  uint8_t prevFrame[HID_FRAME_BYTES] = {0};  // Previous report, for churn detection
  uint8_t volatileMask = 0;                  // Bit i set => byte i free-runs, ignore it
  uint8_t byteChangeCount[HID_FRAME_BYTES] = {0};
  uint8_t byteZeroCount[HID_FRAME_BYTES] = {0};  // Frames in which byte i was 0x00 while learning.
                                                 // A byte that RESTS at zero is a keycode; one that
                                                 // merely visits zero is an axis/counter transiting 0x00.
  unsigned long baselineStartMs = 0;             // Start of the learning window for this shape
  uint16_t baselineFrames = 0;                   // Frames seen during the learning window
  bool baselineReady = false;
  // Quiescence-anchored idle. A clicker transmits only while a button is doing
  // something, so the frame immediately BEFORE a gap in the traffic is the one
  // the report came to rest on — the release. Learning that as idle, instead of
  // whatever frame happens to close the timed window, is what stops a window
  // that closes mid-hold from learning a PRESSED frame as idle (which inverts
  // every signature this shape produces for the rest of the session).
  unsigned long lastFrameMs = 0;             // Arrival of the previous learning frame
  uint8_t restFrame[HID_FRAME_BYTES] = {0};  // Last frame followed by a quiet gap
  bool restFrameValid = false;
  bool active = false;              // Current frame differs from idle on unmasked bytes
  unsigned long activeSinceMs = 0;  // When the current active run began
  uint8_t churnMask = 0;            // Bytes that changed during the current active run
  uint8_t activeChangeCount = 0;    // How many frames changed during the current active run
};

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  unsigned long connectedTime = 0;  // Timestamp when BLE link was established
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect

  // --- Structural press detector ---
  // One learning state per report shape (see PressDetectorState). Which button
  // was pressed is identified by the mapping the user taught us in Bluetooth
  // settings: the wizard captures the signature — the (byte index, value) of
  // the first unmasked byte that left idle — and the decode then matches that
  // signature against each edge frame's CONTENT (frame[index] == value), not
  // against the first-deviating byte, because which byte deviates first depends
  // on the session's learned mask. An unmatched or unmapped press pages
  // forward, so a fresh remote works with no setup. Press dedupe and the
  // signature stay device-level: the same physical press often mirrors onto
  // several report characteristics (even with different shapes) within a few
  // ms, and must collapse into one turn.
  PressDetectorState detectors[MAX_REPORT_SHAPES];
  unsigned long lastPressMs = 0;  // Last injected page turn (repeat suppression)
  unsigned long lastRemoteInputMs = 0;
  // Signature of the most recent accepted press: the first unmasked byte that
  // differed from idle, and its value. 0xFF index = no press seen yet.
  uint8_t lastPressSigIndex = 0xFF;
  uint8_t lastPressSigValue = 0;
  // Whether the most recent press's edge frame matched the learned back
  // signature by CONTENT (see detectPress). The first-deviating-byte signature
  // above is what the wizard captures; this is what the decode consumes.
  bool lastPressIsBack = false;
};

class BluetoothHIDManager {
 public:
  // Singleton access
  static BluetoothHIDManager& getInstance();

  // Lifecycle
  bool enable();
  bool disable();
  bool isEnabled() const { return _enabled; }

  // Scanning
  void startScan(uint32_t durationMs = 10000);
  void stopScan();
  bool isScanning() const { return _scanning; }
  const std::vector<BluetoothDevice>& getDiscoveredDevices() const { return _discoveredDevices; }

  // Connection
  bool connectToDevice(const std::string& address);
  bool disconnectFromDevice(const std::string& address);
  bool isConnected(const std::string& address) const;
  // Cheap "is any remote connected" check (no allocation) — safe to call from
  // the render hot path, unlike getConnectedDevices().
  bool hasConnectedDevice() const { return !_connectedDevices.empty(); }
  std::vector<std::string> getConnectedDevices() const;

  // Input handling
  void processInputEvents();
  void setButtonInjector(std::function<void(uint8_t buttonIndex, bool pressed)> injector);
  void setReaderContextCallback(std::function<bool()> callback);
  // Resolves the physical button index a remote press should be injected as,
  // for either page direction. Supplied by the app because the reader's page
  // side buttons depend on SETTINGS.sideButtonLayout, which lives above this
  // layer. A plain function pointer, not std::function: this is called from the
  // NimBLE task on every press.
  void setPageTurnButtonProvider(uint8_t (*provider)(bool pageForward)) { _pageTurnButtonProvider = provider; }
  // Learned two-button mapping, pushed down from SETTINGS (this layer must not
  // depend on CrossPointSettings). backIndex 0xFF = unmapped: every press pages
  // forward. Only the back signature affects decoding — anything that doesn't
  // match it pages forward — but both are kept so the UI can display the state.
  void setButtonMapping(uint8_t backIndex, uint8_t backValue, uint8_t fwdIndex, uint8_t fwdValue);
  // The mapping wizard turns this on so the presses it captures don't also
  // navigate the menu it is running in. Always turned back off on wizard exit.
  void setInjectionSuppressed(bool suppressed) { _injectionSuppressed = suppressed; }
  // Signature of the most recent press across all remotes, for the mapping
  // wizard. Pair with lastRemoteInputMs() to detect that a fresh press arrived.
  void lastPressSignature(uint8_t& byteIndex, uint8_t& value) const {
    byteIndex = _lastPressSigIndex;
    value = _lastPressSigValue;
  }
  // True once a connected remote's learned idle report shows the stored back
  // signature is unmatchable — the mapping was captured against a bad baseline
  // and every press will page forward until the wizard is re-run.
  bool mappingLooksStale() const { return _mappingLooksStale; }
  void setDebugCaptureEnabled(bool enabled) { _debugCaptureEnabled = enabled; }
  bool isDebugCaptureEnabled() const { return _debugCaptureEnabled; }
  void setBondedDevice(const std::string& address, const std::string& name = "", uint8_t addrType = 0);
  // Called when enable() finds the remembered address is stale and adopts the
  // bond's identity address instead, so the caller can persist the correction.
  // A plain function pointer, not std::function: no capture is needed and this
  // avoids the per-signature heap closure (see CLAUDE.md).
  void setBondedAddressUpdatedCallback(void (*callback)(const char* address, uint8_t addrType));
  void updateActivity();  // Call periodically to check inactivity timeout
  // Reconnect the bonded device when disconnected. Two triggers: a physical
  // button press on the device, or the remote itself advertising (it does so
  // after one of its buttons is pressed) — detected by a passive low-duty
  // background scan that runs whenever enabled+bonded+disconnected.
  void checkAutoReconnect(bool userInputDetected = false);

  // Memory pause: shut the stack down to hand its heap to a memory-critical
  // operation (EPUB section builds need a ~32KB contiguous inflate block).
  // Deliberately does NOT re-enable afterwards — the user turns Bluetooth back
  // on via the reader-menu or settings toggle. Safe to call from the render
  // task: loop-task maintenance is suspended first and the pause is skipped if
  // a reconnect is in flight.
  bool pauseForMemory();  // returns true if the stack was enabled and is now down
  void endMemoryPause();  // clears the paused state (does not enable)
  bool isMemoryPaused() const { return _memoryPaused; }

  // "Bluetooth wanted" session flag. Set when the user turns the remote on
  // (reader menu / settings toggle), cleared when they turn it off or Bluetooth
  // is disallowed (autosync). It SURVIVES every system-driven teardown — a
  // section-build memory pause, leaving and re-entering the reader, a WiFi sync —
  // so the reader's auto-restore can bring the stack back once conditions allow,
  // instead of staying down until a manual re-toggle. Runtime only:
  // never persisted, so a fresh boot starts with Bluetooth off (boot-time BLE
  // reservation was rejected as too costly for non-BLE users).
  void setBluetoothWanted(bool wanted);
  bool isBluetoothWanted() const { return _bluetoothWanted; }

  // Record that WiFi was just powered down (call at a sync's WiFi teardown).
  // Bringing the BT controller up too soon after esp_wifi_stop() hard-freezes
  // this chip, so beginAutoRestoreAttempt() refuses until BLE_WIFI_SETTLE_MS passes.
  void noteWifiActivity();

  // Gate for an automatic restore, called from the reader's idle loop. Returns
  // true (and stamps the rate-limit clock) only when it's a good moment to bring
  // the stack back: wanted, currently down, not mid memory-pause, and the WiFi
  // settle window elapsed. It does NOT enable — the CALLER must first free the
  // heap the controller needs (drop the chapter layout + glyph cache, as the
  // manual toggle does) and then call enable(). This split exists because on the
  // heap-tight X3 the only way to get a >=30KB contiguous block with a book open
  // is to release the resident section first, which only the reader can do.
  bool beginAutoRestoreAttempt();
  // Hold auto-restore off for a while. Set by the reader's degraded-heap render
  // guard after it pauses the stack, so restore doesn't ping-pong against it.
  void deferAutoRestore(unsigned long forMs);

  // Check if BLE has had activity recently (within last 4 minutes)
  // Used by power manager to prevent sleep during BLE use
  bool hasRecentActivity() const;
  // True if a remote sent input within the window. Remote presses are injected as
  // short pulses, so hold-duration is meaningless for them — callers use this to
  // suppress long-press gestures (e.g. chapter skip) right after remote input.
  bool hadRecentRemoteInput(unsigned long windowMs = 1500) const;
  // millis() of the most recent report from any connected remote, or 0 if none
  // has arrived. The settings screen watches this for changes to count
  // individual presses — a decaying window can't distinguish two quick clicks.
  unsigned long lastRemoteInputMs() const;

  // State persistence
  void saveState();
  void loadState();

  std::string lastError;                 // English detail, for logs
  BtStatus lastStatus = BtStatus::None;  // Translatable outcome, for the UI

  // BLE callbacks (public for NimBLE callbacks)
  void onScanResult(NimBLEAdvertisedDevice* advertisedDevice);
  static void onHIDNotify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

 private:
  BluetoothHIDManager();
  ~BluetoothHIDManager();
  BluetoothHIDManager(const BluetoothHIDManager&) = delete;
  BluetoothHIDManager& operator=(const BluetoothHIDManager&) = delete;

  void cleanup();
  void startBackgroundScan();
  // Replace a stale remembered address with the bond store's identity address.
  void reconcileBondedAddressWithStore();
  bool rememberedAddressIsRotating() const;
  void stopBackgroundScan();
  ConnectedDevice* findConnectedDevice(const std::string& address);
  // Feeds one HID report to the device's press detector. Returns true when the
  // frame represents a fresh button press that should turn a page.
  static bool detectPress(ConnectedDevice* device, const uint8_t* data, size_t length, unsigned long nowMs);
  // Detector slot for the given report length; allocate=false only looks up.
  static PressDetectorState* detectorFor(ConnectedDevice* device, size_t length, bool allocate);

  bool _enabled = false;
  bool _scanning = false;
  bool _backgroundScanActive = false;
  // Bounded diagnostic: how many filtered-out advertisers this scan has logged.
  // Capped so a busy RF environment can't flood the serial log (see onScanResult).
  uint8_t _skippedAdvLogs = 0;
  volatile bool _pendingBondedConnect = false;  // set from NimBLE scan callback, consumed in loop task
  // One-shot: BLE just came up with a remote already bonded, so try connecting to
  // its stored address directly instead of waiting to see it advertise. Set in
  // enable(), cleared in disable(), consumed in checkAutoReconnect (loop task).
  bool _pendingEnableConnect = false;
  // Set from the NimBLE scan callback when the bonded remote is recognised by
  // name under a rotated address; consumed in checkAutoReconnect (loop task),
  // which is where it is safe to persist. Fixed buffer, not std::string: this is
  // written from the host task and must not allocate there.
  volatile bool _pendingAddrAdopt = false;
  char _rediscoveredAddr[18] = "";
  std::vector<BluetoothDevice> _discoveredDevices;
  std::vector<ConnectedDevice> _connectedDevices;
  std::function<void(uint8_t, bool)> _buttonInjector;
  std::function<bool()> _readerContextCallback;
  uint8_t (*_pageTurnButtonProvider)(bool pageForward) = nullptr;
  // Learned back-button signature (0xFF index = unmapped). Forward is kept only
  // for state display: decoding is "matches back => back, anything else => forward".
  uint8_t _backSigIndex = 0xFF;
  uint8_t _backSigValue = 0;
  uint8_t _fwdSigIndex = 0xFF;
  uint8_t _fwdSigValue = 0;
  // Set from the NimBLE task when a connected remote's learned idle report
  // proves the stored back signature can never match (see detectPress).
  volatile bool _mappingLooksStale = false;
  // Written from the NimBLE task, read from the loop task. Single-byte/word
  // fields, same cross-task pattern as ConnectedDevice::lastRemoteInputMs.
  volatile uint8_t _lastPressSigIndex = 0xFF;
  volatile uint8_t _lastPressSigValue = 0;
  volatile bool _injectionSuppressed = false;
  bool _debugCaptureEnabled = false;
  bool _memoryPaused = false;
  volatile bool _maintenanceSuspended = false;  // render task asks loop-task maintenance to stand down
  volatile bool _maintenanceBusy = false;       // loop-task maintenance currently executing
  bool _bluetoothWanted = false;                // user asked for the remote; survives system teardowns
  unsigned long _lastWifiActivityMs = 0;        // millis() of the last WiFi power-down; gates re-init
  unsigned long _lastRestoreAttemptMs = 0;      // rate-limits maybeAutoRestore()
  unsigned long _restoreDeferStampMs = 0;       // deferAutoRestore() stamp...
  unsigned long _restoreDeferForMs = 0;         // ...and hold-off duration (0 = none)
  void (*_bondedAddrUpdatedCallback)(const char*, uint8_t) = nullptr;
  std::string _bondedDeviceAddress;
  // BLE_ADDR_PUBLIC/BLE_ADDR_RANDOM. A CONNECT_IND targeting a random static
  // address (e.g. ff:.. clickers) as PUBLIC is ignored by the peer, so the type
  // must survive alongside the address. Loaded from settings at boot and
  // refreshed from the live advertisement by the background reconnect scan.
  uint8_t _bondedAddrType = 0;
  std::string _bondedDeviceName;

  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 300000;  // 5 minutes
  // Minimum quiet time after a WiFi teardown before the BT controller may be
  // re-initialised. The WiFi->BT controller handoff hard-freezes this chip when
  // it happens too soon; a 500ms settle was measured to still freeze, so keep a
  // generous margin. WiFi is fully powered down (WIFI_OFF) at the sync teardown,
  // and this defers the re-init to a later loop iteration rather than firing it
  // immediately after esp_wifi_stop() — the two together are what make it safe.
  static constexpr unsigned long BLE_WIFI_SETTLE_MS = 3000;
  // Backoff between automatic restore attempts. Each attempt (in the reader) frees
  // the chapter layout and re-renders, so a persistently-failing enable must not
  // retry every loop — keep this generous.
  static constexpr unsigned long BLE_RESTORE_RETRY_MS = 5000;
  unsigned long lastMaintenanceCheck = 0;
};

// RAII scope for pauseForMemory()/endMemoryPause(). On destruction the BLE
// stack is left DOWN (if it was paused); it returns lazily on the next physical
// button press or via the reader-menu Bluetooth toggle.
class BleMemoryPause {
 public:
  BleMemoryPause() : paused(BluetoothHIDManager::getInstance().pauseForMemory()) {}
  ~BleMemoryPause() {
    if (paused) {
      BluetoothHIDManager::getInstance().endMemoryPause();
    }
  }
  BleMemoryPause(const BleMemoryPause&) = delete;
  BleMemoryPause& operator=(const BleMemoryPause&) = delete;

 private:
  const bool paused;
};
