#pragma once

#include <Arduino.h>
#include <InputManager.h>

// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define SPI_MISO 7  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 0  // Battery voltage

#define UART0_RXD 20  // Used for USB connection detection

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  // Virtual button overlay (Bluetooth HID injection). Bit N mirrors button index N.
  // desired* is written asynchronously (NimBLE callback context); update() latches it
  // once per frame so activities see a coherent pressed/released edge model.
  uint8_t virtualButtonState = 0;
  uint8_t desiredVirtualButtonState = 0;
  uint8_t previousVirtualButtonState = 0;
  // Presses that arrived since the last update() latch. Some remotes send
  // press and release 1-2ms apart — far inside one loop iteration — so the
  // desired state alone would rise and fall between latches and the press
  // would never be observed. Each pending press is exposed for >=1 frame.
  uint8_t pendingVirtualPresses = 0;
  unsigned long virtualPressStart[7] = {0};
  unsigned long virtualPressFinish[7] = {0};
  unsigned long virtualLastActivityTime[7] = {0};

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }

  // True when the physical side keys sit one per screen edge (left + right),
  // as on the X3 and the X4 Pro; the C3 X4 stacks both on the right edge.
  // Drives button-hint placement, not input mapping.
  inline bool sideKeysAreLeftRight() const {
#if FREEINK_DEVICE_X4PRO
    return true;
#else
    return deviceIsX3();
#endif
  }

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getHeldTime(uint8_t buttonIndex) const;

  // Virtual button injection (for Bluetooth HID remotes)
  void setVirtualButtonState(uint8_t buttonIndex, bool pressed);
  void injectButtonPress(uint8_t buttonIndex);
  void clearVirtualButtons();
  void updateVirtualButtonActivity(uint8_t buttonIndex);

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // Verify power button was held long enough after wakeup.
  // If verification fails, enters deep sleep and does not return.
  // Should only be called when wakeup reason is PowerButton.
  void verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // --- Capacitive touch (X4 Pro) ---------------------------------------------
  // Thin forwarders onto the SDK's InputManager, so nothing above the HAL talks
  // to an SDK class directly. Every one of these is inert on X3/X4: the board
  // profile has no touch controller, so the SDK short-circuits them to false.
  //
  // Coordinates are normalised 0..1 in the panel's NATIVE frame. Nothing consumes
  // them yet — there is no touch UI layer in this build. A future consumer must
  // map them into logical screen space for the current orientation before using
  // them as screen coordinates; the reader rotates at runtime, so the transform is
  // not fixed. The board profile's swapXY/flipX/flipY are already applied by the SDK.

  // True when the board profile carries a touch controller at all.
  bool hasTouch() const;

  // True when the board has the GT911 capacitive home pad.
  bool hasHomeKey() const;

  // A completed tap: finger down and back up inside the tap slop.
  bool wasTouchTap(float& nx, float& ny) const;

  // A completed swipe. Start and end are both reported so the caller can decide
  // direction and whether it began in an edge region.
  bool wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const;

  // Finger currently down and stationary past the long-press threshold.
  bool wasTouchLongPress(float& nx, float& ny) const;

  // Home pad: tap fires on release of a short press; long-press fires while the
  // finger is still down and suppresses the tap.
  bool wasHomeKeyTapped() const;
  bool wasHomeKeyLongPressed() const;

  // Live contact position while a finger is down (no tap-slop gate). Used for
  // gesture calibration and any future drag consumer.
  bool isTouchHeldAt(float& nx, float& ny) const;

  // Any touch contact this frame. Needed by the sleep timer: a touch-only
  // session produces no button press or release, so without this the device
  // would sleep under the user's finger.
  bool wasTouchActivity() const;

  // Drop the remainder of the current contact. Call after consuming a gesture so
  // the finger lift cannot also register as a tap on whatever the gesture opened.
  void suppressTouchContact();

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
