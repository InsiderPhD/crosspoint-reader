#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "SilentRestart.h"
#include "components/UITheme.h"
#include "fontIds.h"

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    // Nothing may read the card once the host has it: SD-backed fonts go first.
    sdFontSystem.releaseForStorage(renderer);
  }

  // Show the safety instructions before giving the raw SD card to the USB host.
  requestUpdateAndWait();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  if (!restartRequested) Storage.endUsbDrive();
  Activity::onExit();
}

void UsbDriveActivity::loop() {
  if (!startFailed) {
    const State nextState = Storage.usbDriveState();
    if (nextState != state) {
      state = nextState;
      requestUpdate();
    }
  }

  // A battery-powered device may stay "mounted" after the cable is pulled: the
  // S3 cannot see VBUS go away in this mode. A sustained bus suspend ends the
  // session (a sleeping host is treated the same way).
  if (state == State::Connected && Storage.usbDriveHostSuspended()) {
    if (!hostSuspendPending) {
      hostSuspendPending = true;
      hostSuspendStartedAt = millis();
    } else if (millis() - hostSuspendStartedAt >= HOST_SUSPEND_TIMEOUT_MS) {
      LOG_INF("USB", "USB Drive host suspend timed out; ending session");
      restartToHome();
      return;
    }
  } else {
    hostSuspendPending = false;
  }

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (!startFailed && state == State::IoError) {
    if (!forcedDisconnectRequested) {
      forcedDisconnectRequested = true;
      forcedDisconnectRequestedAt = millis();
      LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
      if (!Storage.disconnectUsbDriveHost()) {
        LOG_ERR("USB", "Unable to request USB Drive host disconnect");
      }
    } else if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect timed out; forcing restart");
      restartToHome();
    }
    return;
  }

  // Leaving by hand only before a host has the card (or after a failed start):
  // once mounted, the computer must eject it first.
  const bool canExitWithInput = state == State::WaitingForHost || startFailed;
  if (canExitWithInput && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture())) {
    restartToHome();
    return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_USB_DRIVE));

  const char* message = nullptr;
  const char* detail = nullptr;
  const char* secondary = nullptr;
  if (preparing) {
    message = tr(STR_USB_DRIVE_PREPARING);
    detail = tr(STR_USB_DRIVE_EJECT_HINT);
  } else {
    switch (state) {
      case State::WaitingForHost:
        message = tr(STR_USB_DRIVE_WAITING);
        break;
      case State::Connected:
        message = tr(STR_USB_DRIVE_CONNECTED);
        detail = tr(STR_USB_DRIVE_CONNECT_DELAY);
        secondary = tr(STR_USB_DRIVE_EJECT_HINT);
        break;
      case State::IoError:
        message = startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR);
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        break;  // restarting
    }
  }

  // Centred block: the message in bold, then the details, each wrapped.
  const int textWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLine = renderer.getLineHeight(UI_10_FONT_ID);
  std::vector<std::string> msgLines, detailLines, secondaryLines;
  if (message) msgLines = renderer.wrappedText(UI_12_FONT_ID, message, textWidth, 3, EpdFontFamily::BOLD);
  if (detail) detailLines = renderer.wrappedText(UI_10_FONT_ID, detail, textWidth, 4);
  if (secondary) secondaryLines = renderer.wrappedText(UI_10_FONT_ID, secondary, textWidth, 4);
  const int gap = smallLine;
  const int total = static_cast<int>(msgLines.size()) * lineHeight +
                    (detailLines.empty() ? 0 : gap + static_cast<int>(detailLines.size()) * smallLine) +
                    (secondaryLines.empty() ? 0 : gap + static_cast<int>(secondaryLines.size()) * smallLine);
  const int top = metrics.topPadding + metrics.headerHeight;
  const int bottom = pageHeight - metrics.buttonHintsHeight;
  int y = top + std::max(0, (bottom - top - total) / 2);
  for (const auto& line : msgLines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight;
  }
  for (const auto* lines : {&detailLines, &secondaryLines}) {
    if (lines->empty()) continue;
    y += gap;
    for (const auto& line : *lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += smallLine;
    }
  }

  if (state == State::WaitingForHost || startFailed) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  Storage.endUsbDrive();
  delay(20);
  restartToHomeAfterStorageHandoff();
}
