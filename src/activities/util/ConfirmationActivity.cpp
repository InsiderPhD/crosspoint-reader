#include "ConfirmationActivity.h"

#include <I18n.h>

#include "../../components/UITheme.h"
#include "HalDisplay.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(fontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBody = renderer.truncatedText(fontId, body.c_str(), maxWidth, EpdFontFamily::REGULAR);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += lineHeight;
  if (!safeBody.empty()) totalHeight += lineHeight;
  if (!safeHeading.empty() && !safeBody.empty()) totalHeight += spacing;

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  LOG_DBG("CONF", "currentY: %d", currentY);
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight + spacing;
  }

  // Draw Body
  if (!safeBody.empty()) {
    renderer.drawCenteredText(fontId, currentY, safeBody.c_str(), true, EpdFontFamily::REGULAR);
  }

  // Draw UI Elements
#if !FREEINK_DEVICE_X4PRO
  const auto labels = mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
#endif

#if FREEINK_DEVICE_X4PRO
  // No hint bar on the X4 Pro: draw Cancel/Confirm as real buttons. Left key =
  // Cancel = left button, right key = Confirm = right button; in Full Touch
  // they are also tap targets.
  //
  // These stand in for the Full Touch action bar here rather than sitting
  // alongside it (the hint call above is fenced out), so a dialog shows one
  // Cancel/Confirm pair, not two — and so gesture mode, where the bar is not
  // drawn at all, keeps an affordance.
  {
    const int screenW = renderer.getScreenWidth();
    const int y = buttonsY();
    const int cancelX = screenW / 2 - BTN_GAP / 2 - BTN_W;
    const int confirmX = screenW / 2 + BTN_GAP / 2;
    renderer.drawRect(cancelX, y, BTN_W, BTN_H, 2, true);
    renderer.drawRect(confirmX, y, BTN_W, BTN_H, 2, true);
    const char* cancelLabel = I18N.get(StrId::STR_CANCEL);
    const char* confirmLabel = I18N.get(StrId::STR_CONFIRM);
    const int textY = y + (BTN_H - lineHeight) / 2;
    renderer.drawText(fontId, cancelX + (BTN_W - renderer.getTextWidth(fontId, cancelLabel)) / 2, textY, cancelLabel,
                      true);
    renderer.drawText(fontId, confirmX + (BTN_W - renderer.getTextWidth(fontId, confirmLabel)) / 2, textY, confirmLabel,
                      true, EpdFontFamily::BOLD);
  }
#endif

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

#if FREEINK_DEVICE_X4PRO
int ConfirmationActivity::buttonsY() const {
  int contentBottom = startY;
  if (!safeHeading.empty()) contentBottom += lineHeight + spacing;
  if (!safeBody.empty()) contentBottom += lineHeight;
  return contentBottom + spacing;
}

void ConfirmationActivity::finishWith(const bool cancelled) {
  ActivityResult res;
  res.isCancelled = cancelled;
  setResult(std::move(res));
  finish();
}
#endif

void ConfirmationActivity::loop() {
#if FREEINK_DEVICE_X4PRO
  // Taps land on the drawn buttons. In Full Touch mode a tap anywhere else on
  // the screen is Cancel (dialogs must never confirm by accident); in gesture
  // mode a stray tap stays a no-op, matching the old hint-bar behavior.
  {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int screenW = renderer.getScreenWidth();
      const int y = buttonsY();
      const int cancelX = screenW / 2 - BTN_GAP / 2 - BTN_W;
      const int confirmX = screenW / 2 + BTN_GAP / 2;
      const bool inRow = ly >= y && ly < y + BTN_H;
      if (inRow && lx >= confirmX && lx < confirmX + BTN_W) {
        finishWith(/*cancelled=*/false);
        return;
      }
      if (inRow && lx >= cancelX && lx < cancelX + BTN_W) {
        finishWith(/*cancelled=*/true);
        return;
      }
      if (SETTINGS.fullTouchUi) {
        finishWith(/*cancelled=*/true);
        return;
      }
    }
  }
#endif

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}