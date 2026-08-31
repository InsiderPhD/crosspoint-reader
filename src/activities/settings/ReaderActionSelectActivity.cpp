#include "ReaderActionSelectActivity.h"

#if FREEINK_DEVICE_X4PRO

#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ReaderControlsActivity.h"
#include "components/UITheme.h"
#include "util/TouchListNav.h"

namespace {
using A = CrossPointSettings::READER_ACTION;

// One line saying what the action actually does, so a row can be understood
// without binding it and trying it.
const char* actionDescription(const uint8_t action) {
  switch (action) {
    case A::READER_ACTION_PAGE_FORWARD:
      return tr(STR_RA_DESC_PAGE_FORWARD);
    case A::READER_ACTION_PAGE_BACK:
      return tr(STR_RA_DESC_PAGE_BACK);
    case A::READER_ACTION_SKIP_CHAPTER_FORWARD:
      return tr(STR_RA_DESC_CHAPTER_FORWARD);
    case A::READER_ACTION_SKIP_CHAPTER_BACK:
      return tr(STR_RA_DESC_CHAPTER_BACK);
    case A::READER_ACTION_OPEN_MENU:
      return tr(STR_RA_DESC_OPEN_MENU);
    case A::READER_ACTION_GO_HOME:
      return tr(STR_RA_DESC_HOME);
    case A::READER_ACTION_FILE_BROWSER:
      return tr(STR_RA_DESC_FILE_BROWSER);
    case A::READER_ACTION_SYNC:
      return tr(STR_RA_DESC_SYNC);
    case A::READER_ACTION_BOOKMARK:
      return tr(STR_RA_DESC_BOOKMARK);
    case A::READER_ACTION_FORCE_REFRESH:
      return tr(STR_RA_DESC_REFRESH);
    case A::READER_ACTION_DARK_MODE:
      return tr(STR_RA_DESC_DARK_MODE);
    case A::READER_ACTION_SCREENSHOT:
      return tr(STR_RA_DESC_SCREENSHOT);
    case A::READER_ACTION_FOOTNOTES:
      return tr(STR_RA_DESC_FOOTNOTES);
    case A::READER_ACTION_AUTO_PAGE_TURN:
      return tr(STR_RA_DESC_AUTO_PAGE_TURN);
    case A::READER_ACTION_READING_STATS:
      return tr(STR_RA_DESC_READING_STATS);
    case A::READER_ACTION_BIONIC_READING:
      return tr(STR_RA_DESC_BIONIC_READING);
    case A::READER_ACTION_BUTTON_HINTS:
      return tr(STR_RA_DESC_BUTTON_HINTS);
    case A::READER_ACTION_ROTATE_SCREEN:
      return tr(STR_RA_DESC_ROTATE_SCREEN);
    case A::READER_ACTION_CREATE_CLIPPING:
      return tr(STR_RA_DESC_CREATE_CLIPPING);
    case A::READER_ACTION_TOGGLE_BLUETOOTH:
      return tr(STR_RA_DESC_BLUETOOTH);
    case A::READER_ACTION_HIDE_STATUS_BAR:
      return tr(STR_RA_DESC_STATUS_BAR);
    case A::READER_ACTION_DICTIONARY:
      return tr(STR_RA_DESC_DICTIONARY);
    default:
      return tr(STR_RA_DESC_NONE);
  }
}
}  // namespace

ReaderActionSelectActivity::ReaderActionSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       const char* rowTitle, const uint8_t currentAction)
    : Activity("ReaderActionSelect", renderer, mappedInput), currentAction(currentAction) {
  snprintf(this->rowTitle, sizeof(this->rowTitle), "%s", rowTitle ? rowTitle : "");
}

void ReaderActionSelectActivity::onEnter() {
  Activity::onEnter();

  // Same filter the old cycling used: retired values are never offered, and
  // Screenshot is a developer action. A slot already set to Screenshot from a
  // prior Dev session still shows up, so it can be seen and changed.
  const bool dev = SETTINGS.devMode != 0;
  actionCount = 0;
  currentIndex = -1;
  for (uint8_t action = 0; action < CrossPointSettings::READER_ACTION_COUNT; action++) {
    if (CrossPointSettings::isRetiredReaderAction(action)) continue;
    if (!dev && action == CrossPointSettings::READER_ACTION_SCREENSHOT && action != currentAction) continue;
    if (action == currentAction) currentIndex = actionCount;
    actions[actionCount++] = action;
  }

  selectedIndex = currentIndex >= 0 ? currentIndex : 0;
  requestUpdate();
}

void ReaderActionSelectActivity::confirmSelection() {
  setResult(ReaderActionResult{actions[selectedIndex]});
  finish();
}

void ReaderActionSelectActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void ReaderActionSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), actionCount, selectedIndex,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      confirmSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmSelection();
    return;
  }

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  const int pageItems = GUI.listGeometry(listRect(), selectedIndex, /*hasSubtitle=*/true).pageItems;
  int pagedIndex = selectedIndex;
  if (TouchListNav::pageSwipe(mappedInput, actionCount, pageItems, pagedIndex)) {
    selectedIndex = pagedIndex;
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, actionCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, actionCount);
    requestUpdate();
  });
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect ReaderActionSelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void ReaderActionSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // The header names the row being bound ("Tap Left", "Confirm Long press"),
  // which is the only context the picker carries over from Reader Controls.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, rowTitle);

  GUI.drawList(
      renderer, listRect(), actionCount, selectedIndex,
      [this](int index) -> std::string { return ReaderControlsActivity::actionName(static_cast<A>(actions[index])); },
      [this](int index) -> std::string { return actionDescription(actions[index]); }, nullptr,
      [this](int index) -> std::string { return index == currentIndex ? tr(STR_SELECTED) : ""; }, true, nullptr);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // FREEINK_DEVICE_X4PRO
