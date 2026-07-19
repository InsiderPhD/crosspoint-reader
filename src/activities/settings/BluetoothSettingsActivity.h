#pragma once

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>

#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BluetoothSettingsActivity : public Activity {
 private:
  enum class ViewMode { MAIN_MENU, DEVICE_LIST, LEARN_KEYS, DEBUG_MONITOR };

  // Wizard menu rows (LEARN_KEYS view)
  static constexpr int kLearnRowForward = 0;
  static constexpr int kLearnRowBack = 1;
  static constexpr int kLearnRowOneButton = 2;
  static constexpr int kLearnRowSave = 3;
  static constexpr int kLearnRowCount = 4;
  static constexpr size_t kLearnFrameLen = 8;

  ViewMode viewMode = ViewMode::MAIN_MENU;
  int selectedIndex = 0;
  BluetoothHIDManager* btMgr = nullptr;
  std::string lastError = "";
  unsigned long lastScanTime = 0;
  bool learnCapturing = false;  // Confirm pressed on a key row; next remote press is captured
  // Toggle-encoded one-button remotes alternate between two codes per press;
  // when set, both learned codes are treated as page-forward.
  bool learnOneButton = false;

  // Raw HID frame handoff: written from the NimBLE callback, consumed in loop().
  // Single-slot mailbox: `pendingFrameValid` is written last by the producer and
  // cleared first by the consumer.
  volatile bool pendingFrameValid = false;
  uint8_t pendingFrame[kLearnFrameLen] = {0};
  uint8_t pendingFrameLen = 0;

  // Per-press accumulation while capture is armed: bytes that change between the
  // frames of one press (rolling counters, joystick axes) are marked unstable and
  // excluded from the mapping.
  uint8_t capFrame[kLearnFrameLen] = {0};
  bool capStable[kLearnFrameLen] = {false};
  uint8_t capLen = 0;
  uint8_t capCount = 0;
  unsigned long capFirstMs = 0;

  // Captured reference frames per direction.
  uint8_t fwdFrame[kLearnFrameLen] = {0};
  bool fwdStable[kLearnFrameLen] = {false};
  uint8_t fwdLen = 0;
  bool fwdCaptured = false;
  uint8_t backFrame[kLearnFrameLen] = {0};
  bool backStable[kLearnFrameLen] = {false};
  uint8_t backLen = 0;
  bool backCaptured = false;

  // Mapping computed from the two frames: (byte index, value) per direction.
  uint8_t learnedPrevKey = 0;
  uint8_t learnedPrevIdx = 0xFF;
  uint8_t learnedNextKey = 0;
  uint8_t learnedNextIdx = 0xFF;

  uint8_t learnLastTestDir = 0xFF;  // Tester flash: 0x01 = forward, 0x00 = back, 0xFF = none
  unsigned long learnLastTestMs = 0;
  uint16_t debugLastKeycode = 0;
  uint32_t debugEventCount = 0;
  unsigned long debugLastEventMs = 0;
  static constexpr uint8_t kDebugUniqueKeyMax = 8;
  uint8_t debugUniqueKeys[kDebugUniqueKeyMax] = {0};
  uint16_t debugUniqueCounts[kDebugUniqueKeyMax] = {0};
  uint8_t debugUniqueCount = 0;

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
  void handleLearnInput();
  void handleDebugInput();
  void resetLearnState();
  void finalizeLearnCapture();
  bool computeLearnMapping();
  void renderMainMenu();
  void renderDeviceList();
  void renderLearnKeys();
  void renderDebugMonitor();
  std::string getSignalStrengthIndicator(const int32_t rssi) const;
};
