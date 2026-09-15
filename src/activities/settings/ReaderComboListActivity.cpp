#include "ReaderComboListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderComboWizardActivity.h"
#include "ReaderControlsActivity.h"
#include "components/UITheme.h"
#include "util/ReaderCombos.h"
#include "util/TouchListNav.h"

namespace {
constexpr int kRowCount = CrossPointSettings::READER_COMBO_SLOTS;
// Longest chord description: six labels plus five " + " separators.
constexpr size_t COMBO_TEXT_LEN = 64;
}  // namespace

void ReaderComboListActivity::onEnter() {
  Activity::onEnter();
  selectedRow = 0;
  isDirty = false;
  requestUpdate();
}

void ReaderComboListActivity::editSlot(const uint8_t slot) {
  startActivityForResult(
      std::make_unique<ReaderComboWizardActivity>(renderer, mappedInput, slot),
      [this, slot](const ActivityResult& result) {
        const auto* combo = std::get_if<ReaderComboResult>(&result.data);
        if (!combo) return;  // cancelled
        // Binding a chord to None is how a slot is cleared;
        // store the empty form so "not set" has exactly one
        // representation here and on disk.
        const bool cleared = combo->action == CrossPointSettings::READER_ACTION_NONE;
        const uint16_t mask = cleared ? 0 : combo->buttons;
        const uint8_t action = cleared ? CrossPointSettings::READER_ACTION_NONE : combo->action;
        if (SETTINGS.readerComboButtons[slot] == mask && SETTINGS.readerComboAction[slot] == action) {
          return;  // no change, no save
        }
        SETTINGS.readerComboButtons[slot] = mask;
        SETTINGS.readerComboAction[slot] = action;
        // Batched: the single save happens when this screen exits.
        isDirty = true;
      });
}

void ReaderComboListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (isDirty) {
      SETTINGS.saveToFile();
    }
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), kRowCount, selectedRow,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedRow = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedRow = tappedIndex;
      editSlot(static_cast<uint8_t>(selectedRow));
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    editSlot(static_cast<uint8_t>(selectedRow));
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRow = ButtonNavigator::nextIndex(selectedRow, kRowCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedRow = ButtonNavigator::previousIndex(selectedRow, kRowCount);
    requestUpdate();
  });
}

Rect ReaderComboListActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, topOffset, renderer.getScreenWidth(), contentHeight};
}

void ReaderComboListActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOM_COMBOS));

  GUI.drawList(
      renderer, listRect(), kRowCount, selectedRow,
      [](int index) -> std::string {
        const uint16_t mask = SETTINGS.readerComboButtons[index];
        if (mask == 0) return tr(STR_COMBO_NOT_SET);
        char buf[COMBO_TEXT_LEN];
        ReaderCombos::describe(mask, buf, sizeof(buf));
        return buf;
      },
      nullptr, nullptr,
      [](int index) -> std::string {
        if (SETTINGS.readerComboButtons[index] == 0) return "";
        return ReaderControlsActivity::actionName(
            static_cast<CrossPointSettings::READER_ACTION>(SETTINGS.readerComboAction[index]));
      },
      true);

  // Directly under the four rows rather than pinned to the bottom of the list
  // area: the bottom strip there belongs to the page counter.
  const int helpTop = listRect().y + kRowCount * metrics.listRowHeight + 2 * metrics.verticalSpacing;
  GUI.drawHelpText(renderer, Rect{0, helpTop, pageWidth, 20}, tr(STR_COMBO_CLEAR_HINT));

  const auto hints = mappedInput.mapLabels(tr(STR_SAVE_AND_BACK), tr(STR_CHANGE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
