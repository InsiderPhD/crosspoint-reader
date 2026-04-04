#include "HomeBookContextActivity.h"

#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

void HomeBookContextActivity::onEnter() {
  Activity::onEnter();
  // If Confirm is still held from the long-press that opened this menu, ignore its release
  awaitingConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate(true);
}

void HomeBookContextActivity::loop() {
  buttonNavigator.onNext([this] {
    selectorIndex = (selectorIndex + 1) % OPTION_COUNT;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectorIndex = (selectorIndex - 1 + OPTION_COUNT) % OPTION_COUNT;
    requestUpdate();
  });

  if (awaitingConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      awaitingConfirmRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    BookContextResult res;
    res.action = (selectorIndex == OPTION_MARK_AS_READ) ? BookContextResult::Action::MarkAsRead
                                                        : BookContextResult::Action::RemoveFromRecents;
    setResult(ActivityResult{res});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
  }
}

void HomeBookContextActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, tr(STR_BOOK_OPTIONS));

  const int menuTop = metrics.homeTopPadding + metrics.verticalSpacing;
  const int menuHeight = pageHeight - menuTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawButtonMenu(
      renderer, Rect{0, menuTop, pageWidth, menuHeight}, OPTION_COUNT, selectorIndex,
      [](int index) -> std::string {
        return index == 0 ? std::string(tr(STR_MARK_AS_READ)) : std::string(tr(STR_REMOVE_FROM_RECENTS));
      },
      [](int) { return UIIcon::File; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), "", tr(STR_BACK));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
