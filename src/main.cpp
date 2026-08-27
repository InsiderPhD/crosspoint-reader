#include <Arduino.h>
#include <BluetoothHIDManager.h>
#include <BoardConfig.h>
#include <DevicePolicy.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <XteinkDetect.h>  // applyXteinkDisplayController(): per-batch panel controller
#include <builtinFonts/all.h>
#include <esp_log.h>
#include <esp_task_wdt.h>

#include <cstring>

#include "BookFusionTokenStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"  // RenderLock::peek() for the BLE auto-restore render gate
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/HeapReport.h"
#include "util/ScreenshotUtil.h"
#include "util/WifiTimeSync.h"

// Loop-task watchdog budget. Must exceed the longest legitimate single pass of
// loop(): worst-case renders (~30s), a big chapter's section build (~1-2 min).
// Chunked long-runners (HTTP downloads, firmware flashing) feed mid-pass.
constexpr uint32_t LOOP_WDT_TIMEOUT_MS = 300000;  // 5 minutes

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
// Read by the BLE input-mapping callback (NimBLE task context) to decide whether
// generic remote keys mean page-turn (reader) or menu navigation (elsewhere).
static volatile bool gBluetoothReaderContext = false;

void ensureSdFontLoaded() { sdFontSystem.ensureLoaded(renderer); }

// Fonts. bookerly_8 is the default MEDIUM reader font so it stays outside
// OMIT_FONTS (slim build still gets a working reader font).
EpdFont bookerly8RegularFont(&bookerly_8_regular);
EpdFont bookerly8BoldFont(&bookerly_8_bold);
EpdFont bookerly8ItalicFont(&bookerly_8_italic);
EpdFontFamily bookerly8FontFamily(&bookerly8RegularFont, &bookerly8BoldFont, &bookerly8ItalicFont, &bookerly8BoldFont);
#ifndef OMIT_FONTS
EpdFont bookerly6RegularFont(&bookerly_6_regular);
EpdFont bookerly6BoldFont(&bookerly_6_bold);
EpdFont bookerly6ItalicFont(&bookerly_6_italic);
EpdFontFamily bookerly6FontFamily(&bookerly6RegularFont, &bookerly6BoldFont, &bookerly6ItalicFont, &bookerly6BoldFont);
EpdFont bookerly10RegularFont(&bookerly_10_regular);
EpdFont bookerly10BoldFont(&bookerly_10_bold);
EpdFont bookerly10ItalicFont(&bookerly_10_italic);
EpdFontFamily bookerly10FontFamily(&bookerly10RegularFont, &bookerly10BoldFont, &bookerly10ItalicFont,
                                   &bookerly10BoldFont);
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldFont);

// Inter is the sans-serif reader family, normalised to Bookerly sizes (6/8/10/12). Kept
// fully separate from the UI's NotoSans (SMALL_FONT) so regenerating reader fonts never
// touches the UI font.
EpdFont inter6RegularFont(&inter_6_regular);
EpdFont inter6BoldFont(&inter_6_bold);
EpdFont inter6ItalicFont(&inter_6_italic);
EpdFontFamily inter6FontFamily(&inter6RegularFont, &inter6BoldFont, &inter6ItalicFont, &inter6BoldFont);
EpdFont inter8RegularFont(&inter_8_regular);
EpdFont inter8BoldFont(&inter_8_bold);
EpdFont inter8ItalicFont(&inter_8_italic);
EpdFontFamily inter8FontFamily(&inter8RegularFont, &inter8BoldFont, &inter8ItalicFont, &inter8BoldFont);
EpdFont inter10RegularFont(&inter_10_regular);
EpdFont inter10BoldFont(&inter_10_bold);
EpdFont inter10ItalicFont(&inter_10_italic);
EpdFontFamily inter10FontFamily(&inter10RegularFont, &inter10BoldFont, &inter10ItalicFont, &inter10BoldFont);
EpdFont inter12RegularFont(&inter_12_regular);
EpdFont inter12BoldFont(&inter_12_bold);
EpdFont inter12ItalicFont(&inter_12_italic);
EpdFontFamily inter12FontFamily(&inter12RegularFont, &inter12BoldFont, &inter12ItalicFont, &inter12BoldFont);

EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldFont);

EpdFont opendyslexic6RegularFont(&opendyslexic_6_regular);
EpdFont opendyslexic6BoldFont(&opendyslexic_6_bold);
EpdFont opendyslexic6ItalicFont(&opendyslexic_6_italic);
EpdFontFamily opendyslexic6FontFamily(&opendyslexic6RegularFont, &opendyslexic6BoldFont, &opendyslexic6ItalicFont,
                                      &opendyslexic6BoldFont);
EpdFont opendyslexic8RegularFont(&opendyslexic_8_regular);
EpdFont opendyslexic8BoldFont(&opendyslexic_8_bold);
EpdFont opendyslexic8ItalicFont(&opendyslexic_8_italic);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldFont);
EpdFont opendyslexic10RegularFont(&opendyslexic_10_regular);
EpdFont opendyslexic10BoldFont(&opendyslexic_10_bold);
EpdFont opendyslexic10ItalicFont(&opendyslexic_10_italic);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldFont);
EpdFont opendyslexic12RegularFont(&opendyslexic_12_regular);
EpdFont opendyslexic12BoldFont(&opendyslexic_12_bold);
EpdFont opendyslexic12ItalicFont(&opendyslexic_12_italic);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldFont);
EpdFont opendyslexic14RegularFont(&opendyslexic_14_regular);
EpdFont opendyslexic14BoldFont(&opendyslexic_14_bold);
EpdFont opendyslexic14ItalicFont(&opendyslexic_14_italic);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldFont);
#endif  // OMIT_FONTS

EpdFont mono6RegularFont(&mono_6_regular);
EpdFont mono6BoldFont(&mono_6_bold);
EpdFontFamily mono6FontFamily(&mono6RegularFont, &mono6BoldFont, &mono6RegularFont, &mono6BoldFont);
EpdFont mono8RegularFont(&mono_8_regular);
EpdFont mono8BoldFont(&mono_8_bold);
EpdFontFamily mono8FontFamily(&mono8RegularFont, &mono8BoldFont, &mono8RegularFont, &mono8BoldFont);
EpdFont mono10RegularFont(&mono_10_regular);
EpdFont mono10BoldFont(&mono_10_bold);
EpdFontFamily mono10FontFamily(&mono10RegularFont, &mono10BoldFont, &mono10RegularFont, &mono10BoldFont);
EpdFont mono12RegularFont(&mono_12_regular);
EpdFont mono12BoldFont(&mono_12_bold);
EpdFontFamily mono12FontFamily(&mono12RegularFont, &mono12BoldFont, &mono12RegularFont, &mono12BoldFont);

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

void silentRestart() {
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  FsFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation

  // IDF/NimBLE logging routes through log_printfv -> newlib vfprintf, a ~2KB
  // transient stack frame. The sleep sequence already runs loopTask near its 8KB
  // limit (activity teardown -> progress/stats JSON -> SdFat SPI); an IDF log
  // firing at that depth overflowed it by 4 bytes (stack protection fault,
  // 2026-07-29 — only reachable with BLE up, which is the only source of IDF
  // logs in this path). We're powering off: drop IDF logs for the remainder.
  // Runtime-only; logging is back on the wake reboot.
  esp_log_level_set("*", ESP_LOG_NONE);

  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // BLE cannot survive deep sleep; shut the stack down cleanly so the remote
  // disconnects instead of timing out, and NimBLE releases its heap.
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    LOG_INF("SLP", "Disabling Bluetooth before deep sleep");
    btMgr.disable();
  }

  const bool isSeamless = SETTINGS.seamlessSleepScreen == CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_ALWAYS ||
                          (fromTimeout && SETTINGS.seamlessSleepScreen ==
                                              CrossPointSettings::SEAMLESS_SLEEP_SCREEN::SEAMLESS_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isSeamless;

  APP_STATE.saveToFile();

  activityManager.goToSleep(fromTimeout);

  if (isSeamless) {
    saveSleepFrameBuffer();
  }

  halFrontlight.off();
  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // Resolve the panel controller before display.begin() picks a driver. Xteink
  // ships two silicon variants behind one pinout — original units an SSD1677,
  // newer batches an UltraChip UC81xx — and the board profile names SSD1677. Drive
  // a UC81xx unit with SSD1677 commands and the panel simply never answers: BUSY
  // stays low, every wait returns 0 ms, and nothing appears even though the app
  // renders normally.
  //
  // GUARD RATIONALE (mirrors upstream): on the C3 X3/X4, HalGPIO::begin() probes
  // before its own SPI.begin(), and probing again here — after SPI owns the pins —
  // re-resets the panel mid-teardown and freezes the X3. On the S3 X4 Pro that
  // C3-only block is skipped entirely and SPI.begin() happens inside
  // display.begin(), so this is the only place the probe can run in time.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(BOOKERLY_8_FONT_ID, bookerly8FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_6_FONT_ID, bookerly6FontFamily);
  renderer.insertFont(BOOKERLY_10_FONT_ID, bookerly10FontFamily);
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);

  renderer.insertFont(INTER_6_FONT_ID, inter6FontFamily);
  renderer.insertFont(INTER_8_FONT_ID, inter8FontFamily);
  renderer.insertFont(INTER_10_FONT_ID, inter10FontFamily);
  renderer.insertFont(INTER_12_FONT_ID, inter12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(OPENDYSLEXIC_6_FONT_ID, opendyslexic6FontFamily);
  renderer.insertFont(OPENDYSLEXIC_8_FONT_ID, opendyslexic8FontFamily);
  renderer.insertFont(OPENDYSLEXIC_10_FONT_ID, opendyslexic10FontFamily);
  renderer.insertFont(OPENDYSLEXIC_12_FONT_ID, opendyslexic12FontFamily);
  renderer.insertFont(OPENDYSLEXIC_14_FONT_ID, opendyslexic14FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(MONO_6_FONT_ID, mono6FontFamily);
  renderer.insertFont(MONO_8_FONT_ID, mono8FontFamily);
  renderer.insertFont(MONO_10_FONT_ID, mono10FontFamily);
  renderer.insertFont(MONO_12_FONT_ID, mono12FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

#if FREEINK_DEVICE_X4PRO
  // FIRST statement in setup(), before serial, before gpio.begin(), before any
  // delay: on the X4 Pro the master peripheral rail is a latch on GPIO1
  // (XTEINK_X4_PRO's power.latch0), and until firmware drives it HIGH the board
  // stays up only while the power button is physically bridging the rail. On USB
  // that goes unnoticed because VBUS holds everything up; on battery the device
  // dies the moment the button is released, which reads as "won't boot".
  // Anything that delays this — the 200 ms serial settle below included — is
  // that much longer the user has to keep holding the button.
  //
  // holdPowerRails() normally rides in on BoardConfig::selectDevice(), but that
  // is only called for the X3 (see below); the X4 Pro is DEFAULT_DEVICE and so
  // never passes through it. Calling it directly is idempotent with that path.
  //
  // X3/X4 are deliberately excluded: their latch is GPIO13 (the battery MOSFET
  // on X4, the SD rail on X3), those units self-latch through the button pull,
  // and asserting it early would change boot on shipping hardware to fix a bug
  // they do not have.
  BoardConfig::holdPowerRails();
#endif

#ifdef ENABLE_SERIAL_LOG
  // Bring serial up FIRST, before any hardware init. The regular Serial.begin()
  // below sits after gpio/power/tilt/clock init, so a hang in any of those
  // produces an enumerated USB CDC port that never prints a byte — silence that
  // is indistinguishable from a dead device. Starting here means early bring-up
  // failures are visible. Costs a few ms and one duplicate begin(), which is
  // harmless. (Ordering matters on the X4 Pro, where all of that init is new.)
  Serial.begin(115200);
  delay(200);  // let the host re-open the CDC endpoint before the first write
  LOG_INF("BOOT", "=== setup() entry: serial up before hardware init ===");
#endif

  // Watch the loop task: a wedged main loop (observed with the BLE controller
  // starved at low CPU frequency) otherwise leaves the device in limbo forever —
  // buttons are software-polled and deep sleep is software-entered, so neither
  // works, and the battery drains until a manual reset. With the watchdog, a
  // wedge panics (capturing a trace) and reboots into working firmware, where
  // the normal inactivity timeout then puts the device to sleep properly.
  // 5 minutes tolerates every legitimate long block of the loop task (renders,
  // section builds, syncs); the truly long runners (downloads, firmware
  // flashing) additionally feed the watchdog per chunk.
  {
    esp_task_wdt_config_t wdtConfig = {};
    wdtConfig.timeout_ms = LOOP_WDT_TIMEOUT_MS;
    wdtConfig.idle_core_mask = 0;
    wdtConfig.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK) {
      esp_task_wdt_init(&wdtConfig);  // TWDT not pre-initialized in this build
    }
    enableLoopWDT();  // subscribes loopTask; the core feeds it every loop() pass
  }

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();

  // Lock in the runtime board profile right after hardware detection, before any
  // SDK driver reads BoardConfig::ACTIVE. The status-bar clock uses freeink::Rtc,
  // which reads the RTC address/bus from ACTIVE; without this, ACTIVE stays at the
  // X4 default until display init (setDisplayX3) and halClock.begin() would see "no
  // RTC". X4 is the default profile and is not selected here, so the X4 path is
  // unchanged. selectDevice() is idempotent, so the later setDisplayX3() is harmless.
  if (gpio.deviceIsX3()) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3);
  }

  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

#ifdef ENABLE_SERIAL_LOG
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    const unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
#endif

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.loadSettings();
  KOREADER_STORE.loadFromFile();
  UITheme::getInstance().reload();
  // Frontlight (X4 Pro): restore the saved brightness/warmth now that settings
  // are loaded. No-op on boards without one.
  halFrontlight.begin();
  halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);
  ButtonNavigator::setMappedInputManager(mappedInputManager);
#if FREEINK_DEVICE_X4PRO
  mappedInputManager.setRenderer(&renderer);
#endif

  // Bluetooth HID page-turner remotes: a remote press is injected as a virtual
  // press of a physical side button, so activities see it as normal input. With
  // no learned mapping every press pages forward; a press matching the mapped
  // back-button signature pages back. The stack itself is only initialised when
  // the user enables Bluetooth.
  auto& btMgr = BluetoothHIDManager::getInstance();
  btMgr.setButtonInjector([](uint8_t buttonIndex, bool pressed) { gpio.setVirtualButtonState(buttonIndex, pressed); });
  btMgr.setReaderContextCallback([]() { return gBluetoothReaderContext; });
  // The page buttons live on side buttons that the user can swap, so resolve
  // them through the same mapping the reader uses rather than hardcoding.
  btMgr.setPageTurnButtonProvider([](bool pageForward) {
    return mappedInputManager.getPhysicalButtonIndex(pageForward ? MappedInputManager::Button::PageForward
                                                                 : MappedInputManager::Button::PageBack);
  });
  btMgr.setBondedDevice(SETTINGS.bleBondedDeviceAddr, SETTINGS.bleBondedDeviceName, SETTINGS.bleBondedDeviceAddrType);
  // The pairing screen stores the address the scan reported, which for a remote
  // using a Resolvable Private Address goes stale on the remote's own rotation
  // timer. When enable() corrects it from the bond store, persist the correction
  // so the next boot starts from the durable identity address. Guarded on a real
  // change: this must not turn into a settings write on every enable.
  btMgr.setBondedAddressUpdatedCallback([](const char* address, uint8_t addrType) {
    if (strcmp(SETTINGS.bleBondedDeviceAddr, address) == 0 && SETTINGS.bleBondedDeviceAddrType == addrType) {
      return;
    }
    strncpy(SETTINGS.bleBondedDeviceAddr, address, sizeof(SETTINGS.bleBondedDeviceAddr) - 1);
    SETTINGS.bleBondedDeviceAddr[sizeof(SETTINGS.bleBondedDeviceAddr) - 1] = '\0';
    SETTINGS.bleBondedDeviceAddrType = addrType;
    SETTINGS.saveToFile();
  });
  btMgr.setButtonMapping(SETTINGS.bleBackSigIndex, SETTINGS.bleBackSigValue, SETTINGS.bleFwdSigIndex,
                         SETTINGS.bleFwdSigValue);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
#if FREEINK_DEVICE_X4PRO
      // NOT verified on the X4 Pro: the check would deep-sleep the device on
      // every battery boot, and it could never get back out.
      //
      // verifyPowerButtonWakeup() sleeps the device unless the power button is
      // STILL held when it runs — ~700ms into boot, plus up to 1s of polling.
      // On X3/X4 that holds by construction: deep sleep drives GPIO13 low to
      // disconnect the battery through the protection MOSFET, so the rail is up
      // only while the button is pressed and firmware cannot reach this line
      // otherwise. The X4 Pro has no such latch (GPIO13 is the display CS —
      // see HalPowerManager::startDeepSleep), so a normal short press is long
      // released by now and reads as a spurious wake.
      //
      // The failure is self-sustaining: with no battery cut, the sleep it
      // triggers is answered by another cold POWERON, which lands here again.
      // Symptom is a device that boots on USB and is dead on battery.
      //
      // Cold power-on is therefore taken at face value here. The anti-pocket-wake
      // guard this gives up needs a hold gate that survives to the check —
      // latching the press in HalGPIO at boot, not re-reading the pin later.
      LOG_DBG("MAIN", "Cold boot on battery: power button press taken as intentional");
#else
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
#endif
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts(/*seamless=*/!APP_STATE.showBootScreen);

  if (!isSilentReboot) {
    if (APP_STATE.showBootScreen) {
      activityManager.goToBoot();
    } else if (loadSleepFrameBuffer()) {
      // Seamless wake: buffer restored, replace moon icon with loading icon
      const auto pageHeight = renderer.getScreenHeight();
      renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
    } else {
      // Frame buffer file missing — fall back to normal boot screen
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      activityManager.goToBoot();
    }
  } else {
    // The panel physically kept the pre-restart screen (e.g. the web server page)
    // through ESP.restart(), and the driver's implicit cold-start clean is only the
    // stock single-pass HALF — high-contrast content ghosts through it. Wipe with
    // the multi-flash FULL waveform so the destination paints on a clean panel.
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }

  READING_STATS.loadFromFile();
  BF_TOKEN_STORE.loadFromFile();

  // Silent NTP attempt against the last-connected WiFi network, on a background
  // task — non-blocking. Up to 3 SNTP retries, ~10s worst case, no UI shown.
  // If it succeeds, the header date "?" disappears and any reading recorded
  // during this boot lands on the correct calendar day.
  WifiTimeSync::startSilentBootAttempt();

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (isSilentReboot && snapshotTarget == SILENT_REBOOT_TARGET_READER && !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (isSilentReboot) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

#if FREEINK_DEVICE_X4PRO
  // Activities that own touch input (the readers' tap zones and home-key
  // actions, the keyboard's tap hit-testing) opt out of the global
  // tap-anywhere-is-Confirm and home-key-is-Confirm conveniences; every other
  // screen — including the reader's own menus — gets both.
  const bool activityOwnsTouch = activityManager.consumesTouchInput();
  // Full Touch mode: converted screens hit-test taps against their drawn UI,
  // so only the tap injection is disabled for them — a home-key tap still
  // means Confirm there. One-frame lag on modal open/close is safe: the
  // contact that opens a modal is suppressed by the opener.
  const bool directTouch = SETTINGS.fullTouchUi && activityManager.handlesDirectTouch();
  mappedInputManager.setHomeKeyActsAsConfirm(!activityOwnsTouch);
  mappedInputManager.setTapActsAsConfirm(!activityOwnsTouch && !directTouch);
  // Full Touch keeps only the Back swipe as an injected press everywhere except
  // the actual reading screens, whose swipe/tap actions the user configures
  // explicitly in Reader Controls. Those are the activities that are BOTH
  // isReaderActivity AND consumesTouchInput — reader sub-screens (menu,
  // chapters, bookmarks) claim reader status only for the Bluetooth lifecycle
  // and are plain UI, so the Back-only rule applies to them like any other
  // screen. The up/down/right swipes on those screens are recorded rather than
  // injected: lists that actually paginate turn a page with up/down
  // (TouchListNav::pageSwipe) and the tabbed screens take the next tab from
  // right (TouchListNav::tabSwipeNext).
  const bool onReadingScreen = activityManager.isReaderActivity() && activityOwnsTouch;
  mappedInputManager.setSwipesBackOnly(SETTINGS.fullTouchUi && !onReadingScreen);
#endif
  // Latches buttons (gpio.update()) and, on X4 Pro, classifies touch swipes
  // into synthesized button presses.
  mappedInputManager.update();

  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  // Bluetooth HID maintenance: inactivity timeout and bonded-remote reconnect.
  // Cheap no-ops while Bluetooth is disabled.
  gBluetoothReaderContext = activityManager.isReaderActivity();
  auto& btMgr = BluetoothHIDManager::getInstance();
  // wasTouchActivity() is compiled to false on boards without a touch panel.
  const bool physicalInputDetected = gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity();
  // Bluetooth only runs where it's used: the reader and the Bluetooth settings
  // screen. Everywhere else the stack is shut down so its heap (and the
  // fragmentation it causes) goes back to the rest of the firmware. Autosync now
  // coexists with the remote: when a background push fires the reader tears the
  // BLE stack down for the WiFi session and auto-restores it afterwards (see
  // EpubReaderActivity::runAutosyncNow), so nothing needs to be locked out here.
  const bool bleAllowedHere = activityManager.keepsBluetoothActive();
  // The stack goes down for many reasons (leaving the reader, a section-build
  // memory pause, a WiFi sync, deep sleep). When it does, the "Bluetooth wanted"
  // session flag survives so the stack can be brought back without a manual
  // re-toggle. The restore itself lives in the reader (EpubReaderActivity, driven
  // off beginAutoRestoreAttempt()), because on the heap-tight X3 re-enabling with a
  // book open requires releasing the resident chapter layout first — something only
  // the reader can do. Here we only power the stack DOWN when it's not allowed.
  if (btMgr.isEnabled() && !bleAllowedHere) {
    // Leaving the reader/BT settings: power the stack down immediately so its
    // ~56KB is back in the heap before whatever comes next (home browsing, and
    // especially a book open with its spine/section cache builds).
    LOG_INF("MAIN", "Disabling Bluetooth (outside reader/settings)");
    btMgr.disable();
  }
  btMgr.updateActivity();
  btMgr.checkAutoReconnect(physicalInputDetected);
  const bool bleRecentActivity = btMgr.isEnabled() && btMgr.hasRecentActivity();

  renderer.setFadingFix(SETTINGS.fadingFix);
  // Suppress images whenever Bluetooth is in play — the stack being UP *or* the user
  // wanting it (auto-restore may have it momentarily down for a section build). Two
  // reasons: (1) a connected remote leaves ~10-20KB free and the JPEG decoder wants
  // ~36KB, so the decode would fail ten times per page; (2) more importantly, the
  // decode fragments the heap into the 20-30KB zone where a following BLE controller
  // init HANGS (frozen device). Gating on "wanted" means the decoder never runs while
  // a remote is in use, so the heap stays defragmented and auto-restore's enable()
  // always finds a clean contiguous block. Suppression is render-time only (layout
  // reserved the space at build time), so it neither invalidates the section cache
  // nor changes the page — the image area is simply left blank.
#if CROSSPOINT_BLE_EXCLUSIVE
  renderer.setImagesSuppressed(btMgr.isEnabled() || btMgr.isBluetoothWanted());
#else
  // Roomy board: the JPEG decoder's ~36KB is not in competition with the BLE
  // stack here, and there is no controller-init cliff to keep the heap
  // defragmented for, so book images render with a remote connected.
  renderer.setImagesSuppressed(false);
#endif

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Touch counts as activity: without it a touch-only session would hit the
  // sleep timer under the user's finger (sub-threshold drags inject no press).
  const bool userInputDetected = gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity();

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (userInputDetected || halTiltSensor.hadActivity() || activityManager.preventAutoSleep() || bleRecentActivity) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // RAM investigator: POWER + Confirm, alongside the POWER + DOWN screenshot
  // chord. Handled here rather than in an activity so a heap reading can be
  // taken on any screen, including mid-render states no menu can reach.
  //
  // Deliberately NOT POWER + UP: that is the recovery-firmware chord checked
  // during setup() (see above), and putting a second meaning on it invites a
  // mis-press near the SD firmware update path. Confirm is resolved through
  // MappedInputManager so the chord follows the user's front-button remapping.
  static bool heapReportButtonsReleased = true;
  static bool heapReportComboActive = false;
  const bool heapReportChordHeld =
      gpio.isPressed(HalGPIO::BTN_POWER) && mappedInputManager.isPressed(MappedInputManager::Button::Confirm);
  if (heapReportChordHeld) {
    heapReportComboActive = true;
    if (heapReportButtonsReleased) {
      heapReportButtonsReleased = false;
      {
        RenderLock lock;
        HeapReport::dump(renderer);
      }
    }
    return;
  }
  if (heapReportComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      heapReportButtonsReleased = true;
      heapReportComboActive = false;
      return;
    }
    heapReportButtonsReleased = true;
    heapReportComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Sleep requires a 500ms hold so that a short press can be used by UI elements
  // (e.g. the sort menu) without accidentally putting the device to sleep.
  // getPowerButtonDuration() (10ms) is intentionally kept short for wake-from-sleep
  // verification only; the sleep trigger needs its own, longer threshold.
  static constexpr unsigned long SLEEP_HOLD_MS = 500;
  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SLEEP_HOLD_MS) {
    // If a POWER chord is potentially being pressed (screenshot / RAM
    // investigator), don't sleep.
    if (gpio.isPressed(HalGPIO::BTN_DOWN) || mappedInputManager.isPressed(MappedInputManager::Button::Confirm)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

#if FREEINK_DEVICE_X4PRO
  // Collapse the panel's analog rails once no refresh has happened for a few
  // seconds. Between refreshes the UC8179 otherwise sits powered (PON with no
  // matching POF), holding VCOM/VGH/VGL applied for as long as a static screen
  // is up — which shows up as the classic left-powered image drift/artifacts.
  // Keyed to msSinceLastRefresh(), NOT the activity/sleep timer: activities
  // with preventAutoSleep() (downloads, web server, WiFi flows) reset the
  // activity timer every pass and would otherwise never let the rails drop
  // under their long static screens. Controller RAM survives POF, so the next
  // paint only pays a PON and still diffs against its retained OLD plane.
  //
  // Skipped rather than queued while the render task holds the lock: blocking
  // the loop task behind a ~1 s page render for an opportunistic power-off is
  // a bad trade. The next pass retries.
  if (display.msSinceLastRefresh() > 3000 && !RenderLock::peek()) {
    RenderLock lock;
    display.powerOffIdle();
  }

  // Static-screen maintenance: fast/partial waveforms (and the AA gray overlay
  // even more so) are not DC-balanced, and the residual charge they leave
  // relaxes into random speckle over minutes on a static screen — with the
  // rails already collapsed, so this is bistability decay, not a powered
  // panel. Page turns and menu paints scrub it; a screen nobody touches never
  // gets scrubbed. Arm a full GC flash and repaint the current activity
  // through its normal render path (NOT a raw framebuffer redisplay, which
  // would flatten AA text and grayscale covers to B/W). Costs one full-refresh
  // flash per interval; the sleep timer is untouched, so a forgotten device
  // still auto-sleeps on schedule.
  {
    static constexpr unsigned long STATIC_SCREEN_MAINT_MS = 5UL * 60UL * 1000UL;
    static unsigned long lastMaintRequestMs = 0;
    if (display.msSinceLastRefresh() > STATIC_SCREEN_MAINT_MS &&
        millis() - lastMaintRequestMs > STATIC_SCREEN_MAINT_MS && !RenderLock::peek()) {
      lastMaintRequestMs = millis();
      LOG_DBG("SLP", "Static-screen maintenance: full-refresh repaint");
      display.requestResync(1);
      activityManager.requestUpdate();
    }
  }
#endif

  // Long-press Back from any activity returns to the home screen. Fires once per hold
  // cycle (resets on release) so a continued hold doesn't keep retriggering, and
  // individual activities don't need their own copy of this gesture.
  static bool longBackHomeFired = false;
  if (mappedInputManager.isPressed(MappedInputManager::Button::Back)) {
    if (!longBackHomeFired && mappedInputManager.getHeldTime() >= 1000UL) {
      longBackHomeFired = true;
      activityManager.goHome();
    }
  } else {
    longBackHomeFired = false;
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
