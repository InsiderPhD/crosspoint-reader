#include "BatteryActivity.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>

#include "BatteryInsightsView.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

void BatteryActivity::onEnter() {
  Activity::onEnter();
  // Read once here, on the loop task: the header's battery icon reads the
  // gauge from the render task, and the two must not interleave on the X3's I2C.
  batteryPercent = static_cast<uint8_t>(std::min<uint16_t>(powerManager.getBatteryPercentage(), 100));
  requestUpdate();
}

void BatteryActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void BatteryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BATTERY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  BatteryInsightsView::draw(renderer, metrics.contentSidePadding, contentTop,
                            pageWidth - metrics.contentSidePadding * 2, contentBottom - contentTop, batteryPercent);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
