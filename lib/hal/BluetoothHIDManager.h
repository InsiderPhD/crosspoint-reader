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
};

// Number of leading HID report bytes the press detector considers. Every clicker
// we have seen encodes its button state well inside this window, and the fixed
// size keeps ConnectedDevice free of heap allocations.
inline constexpr size_t HID_FRAME_BYTES = 8;

struct ConnectedDevice {
  std::string address;
  std::string name;
  NimBLEClient* client = nullptr;
  std::vector<NimBLERemoteCharacteristic*> reportChars;
  unsigned long connectedTime = 0;  // Timestamp when BLE link was established
  bool subscribed = false;
  unsigned long lastActivityTime = 0;  // Timestamp of last HID report received
  bool wasConnected = false;           // Track if this device was previously connected for auto-reconnect

  // --- Direction-agnostic press detector state ---
  // Any button on any remote turns one page forward, so we never decode keycodes.
  // Instead we learn what this remote's idle report looks like, mask off the bytes
  // that free-run (rolling counters, joystick axes), and treat every idle -> active
  // transition of the remaining bytes as one press.
  uint8_t idleFrame[HID_FRAME_BYTES] = {0};  // Reference "nothing pressed" report
  uint8_t prevFrame[HID_FRAME_BYTES] = {0};  // Previous report, for churn detection
  uint8_t volatileMask = 0;                  // Bit i set => byte i free-runs, ignore it
  uint8_t byteChangeCount[HID_FRAME_BYTES] = {0};
  unsigned long baselineStartMs = 0;  // Start of the post-connect learning window
  uint16_t baselineFrames = 0;        // Frames seen during the learning window
  bool baselineReady = false;
  bool active = false;              // Current frame differs from idle on unmasked bytes
  unsigned long activeSinceMs = 0;  // When the current active run began
  uint8_t churnMask = 0;            // Bytes that changed during the current active run
  uint8_t activeChangeCount = 0;    // How many frames changed during the current active run
  unsigned long lastPressMs = 0;    // Last injected page turn (repeat suppression)
  unsigned long lastRemoteInputMs = 0;
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
  std::vector<std::string> getConnectedDevices() const;

  // Input handling
  void processInputEvents();
  void setButtonInjector(std::function<void(uint8_t buttonIndex, bool pressed)> injector);
  void setReaderContextCallback(std::function<bool()> callback);
  // Resolves the physical button index a remote press should be injected as.
  // Supplied by the app because the reader's page-forward side button depends on
  // SETTINGS.sideButtonLayout, which lives above this layer. A plain function
  // pointer, not std::function: this is called from the NimBLE task on every press.
  void setPageTurnButtonProvider(uint8_t (*provider)()) { _pageTurnButtonProvider = provider; }
  void setDebugCaptureEnabled(bool enabled) { _debugCaptureEnabled = enabled; }
  bool isDebugCaptureEnabled() const { return _debugCaptureEnabled; }
  void setBondedDevice(const std::string& address, const std::string& name = "");
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
  void stopBackgroundScan();
  ConnectedDevice* findConnectedDevice(const std::string& address);
  // Feeds one HID report to the device's press detector. Returns true when the
  // frame represents a fresh button press that should turn a page.
  static bool detectPress(ConnectedDevice* device, const uint8_t* data, size_t length, unsigned long nowMs);

  bool _enabled = false;
  bool _scanning = false;
  bool _backgroundScanActive = false;
  volatile bool _pendingBondedConnect = false;  // set from NimBLE scan callback, consumed in loop task
  std::vector<BluetoothDevice> _discoveredDevices;
  std::vector<ConnectedDevice> _connectedDevices;
  std::function<void(uint8_t, bool)> _buttonInjector;
  std::function<bool()> _readerContextCallback;
  uint8_t (*_pageTurnButtonProvider)() = nullptr;
  bool _debugCaptureEnabled = false;
  bool _memoryPaused = false;
  volatile bool _maintenanceSuspended = false;  // render task asks loop-task maintenance to stand down
  volatile bool _maintenanceBusy = false;       // loop-task maintenance currently executing
  std::string _bondedDeviceAddress;
  std::string _bondedDeviceName;

  // Inactivity timeout (milliseconds)
  static constexpr unsigned long INACTIVITY_TIMEOUT_MS = 300000;  // 5 minutes
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
