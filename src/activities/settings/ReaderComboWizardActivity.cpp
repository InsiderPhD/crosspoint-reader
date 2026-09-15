#include "ReaderComboWizardActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderActionSelectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReaderCombos.h"

namespace {
// Wizard steps, in the order they are drawn as rows.
constexpr int kStepCount = 2;
constexpr int kStepButtons = 0;
constexpr int kStepAction = 1;
// How long a "hold at least two" / "already used" banner stays up.
constexpr unsigned long MESSAGE_MS = 1800;
// Longest chord description: six labels plus five " + " separators.
constexpr size_t COMBO_TEXT_LEN = 64;

// What to hold, and how to get out, both depend on what the board actually
// has: front buttons on the X3/X4, two side keys plus the screen on the X4 Pro.
const char* kPrompt() {
#if FREEINK_DEVICE_X4PRO
  return tr(STR_COMBO_PROMPT_TOUCH);
#else
  return tr(STR_COMBO_PROMPT);
#endif
}
#if !FREEINK_DEVICE_X4PRO
// X3/X4 only: those boards draw no hint bar here (every front button is a
// capture target), so the way out has to be said in words. The X4 Pro shows a
// real Back button instead.
const char* kCancelHint() { return tr(STR_COMBO_CANCEL_HINT); }
#endif
}  // namespace

ReaderComboWizardActivity::ReaderComboWizardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const uint8_t slot)
    : Activity("ReaderComboWizard", renderer, mappedInput), slot(slot) {}

void ReaderComboWizardActivity::onEnter() {
  Activity::onEnter();
  step = Step::Capture;
  captureMask = 0;
  gripStarted = false;
  armed = false;
  message = nullptr;
  messageUntil = 0;
  requestUpdate();
}

void ReaderComboWizardActivity::restartCapture(const char* newMessage) {
  step = Step::Capture;
  captureMask = 0;
  gripStarted = false;
  // Whatever ended the last attempt is probably still under a finger — the
  // Back that backed out of the picker, or the keys of a rejected chord.
  armed = false;
  message = newMessage;
  messageUntil = newMessage ? millis() + MESSAGE_MS : 0;
  requestUpdate();
}

void ReaderComboWizardActivity::cancel() {
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}

void ReaderComboWizardActivity::openActionPicker() {
  char title[COMBO_TEXT_LEN];
  ReaderCombos::describe(captureMask, title, sizeof(title));
  // Seeded with whatever this slot runs today, so re-binding a combo opens the
  // picker on its current action rather than at the top of the list.
  startActivityForResult(
      std::make_unique<ReaderActionSelectActivity>(renderer, mappedInput, title, SETTINGS.readerComboAction[slot]),
      [this](const ActivityResult& result) {
        const auto* picked = std::get_if<ReaderActionResult>(&result.data);
        if (!picked) {
          // Backed out of the picker: keep the wizard open and re-record the
          // chord, rather than storing a combo the user never gave an action.
          restartCapture(nullptr);
          return;
        }
        setResult(ReaderComboResult{captureMask, picked->action});
        finish();
      });
}

void ReaderComboWizardActivity::loop() {
  if (messageUntil != 0 && millis() > messageUntil) {
    message = nullptr;
    messageUntil = 0;
    requestUpdate();
  }

  // The picker is on top of us; nothing here runs until it returns.
  if (step == Step::PickAction) return;

#if FREEINK_DEVICE_X4PRO
  // The way out on a board with no front buttons: the action bar's Back slot,
  // or the left swipe that injects the same press. Checked before `armed` so a
  // user who cannot work out what to hold is never stuck on this screen.
  // Reading Back here is safe only because this board cannot capture it -- see
  // ReaderCombos::eligibleMask. The home key USED to cancel here and no longer
  // can: it is a chord member now, so a tap on it has to reach the capture.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }
#endif

  // uint16_t, not uint8_t: the zone bits live above bit 7 and a narrower local
  // would drop every one of them.
  const uint16_t live = ReaderCombos::currentMask(mappedInput);

  if (!armed) {
    // Wait for a clean slate. The Confirm that opened this screen is still
    // down on the first frame, and would otherwise join the chord.
    if (live == 0) armed = true;
    return;
  }

  if (live != 0) {
#if FREEINK_DEVICE_X4PRO
    // A finger that crosses from one third into another is a swipe, not a grip,
    // and unioning its zones would record a chord of all three. heldTouchZone()
    // has no tap-slop gate — it follows a moving finger — so this screen owns
    // the "hold still" rule.
    const uint16_t liveZone = live & ReaderCombos::zoneMask();
    const uint16_t heldZone = captureMask & ReaderCombos::zoneMask();
    if (liveZone != 0 && heldZone != 0 && liveZone != heldZone) {
      restartCapture(tr(STR_COMBO_HOLD_STILL));
      return;
    }
#endif
    gripStarted = true;
    const uint16_t merged = static_cast<uint16_t>(captureMask | live);
    if (merged != captureMask) {
      captureMask = merged;
      requestUpdate();  // show the chord growing under the user's fingers
    }
    return;
  }

  // All keys up. Everything held during the grip is the chord.
  if (!gripStarted) return;
  gripStarted = false;

  if (ReaderCombos::buttonCount(captureMask) >= 2) {
    if (ReaderCombos::slotForMask(captureMask, static_cast<int8_t>(slot)) >= 0) {
      restartCapture(tr(STR_COMBO_IN_USE));
      return;
    }
    step = Step::PickAction;
    openActionPicker();
    return;
  }

  // One input on its own is not a chord. On the X3/X4, Back alone is the way
  // out: every other button is a capture target here, so there is no spare one
  // to cancel with. (On the X4 Pro the Back bit is never capturable — that
  // board cancels with the home key, handled above.)
  if (captureMask == static_cast<uint16_t>(1u << CrossPointSettings::COMBO_BTN_BACK)) {
    cancel();
    return;
  }
  restartCapture(tr(STR_COMBO_NEED_TWO));
}

#if FREEINK_DEVICE_X4PRO
void ReaderComboWizardActivity::drawZoneMap(const Rect& area) const {
  if (area.height <= 0) return;

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const bool bands = SETTINGS.readerTapZoneLayout == CrossPointSettings::TAP_ZONES_TOP_BOTTOM;

  // Bit order matches the screen order in both layouts: Left is the top band
  // under TAP_ZONES_TOP_BOTTOM (see MappedInputManager::TapZone).
  constexpr uint8_t kZoneBits[3] = {CrossPointSettings::COMBO_BTN_ZONE_LEFT, CrossPointSettings::COMBO_BTN_ZONE_MIDDLE,
                                    CrossPointSettings::COMBO_BTN_ZONE_RIGHT};
  // The boundaries classifyZone() actually uses: thirds of the whole logical
  // frame. Under TAP_ZONES_TOP_BOTTOM the first band starts above `area` (the
  // header sits in it), so each band is clipped to what is on show here.
  const int cut1 = bands ? screenH / 3 : screenW / 3;
  const int cut2 = bands ? (2 * screenH) / 3 : (2 * screenW) / 3;
  const int starts[3] = {0, cut1, cut2};
  const int ends[3] = {cut1, cut2, bands ? screenH : screenW};

  const int areaBottom = area.y + area.height;
  const int lineHeight = renderer.getTextHeight(UI_10_FONT_ID);

  for (int i = 0; i < 3; i++) {
    int x = area.x;
    int y = area.y;
    int w = area.width;
    int h = area.height;
    if (bands) {
      y = starts[i] > area.y ? starts[i] : area.y;
      const int bottom = ends[i] < areaBottom ? ends[i] : areaBottom;
      h = bottom - y;
    } else {
      x = starts[i];
      w = ends[i] - starts[i];
    }
    if (w <= 0 || h <= 0) continue;  // band entirely above the mapped area

    // Dither rather than invert: the label has to stay readable, and this is
    // the same LightGray fill the themes use for a selected cell. Outlined too,
    // so "you are holding this one" survives a glance on a slow panel.
    if (captureMask & (1u << kZoneBits[i])) {
      renderer.fillRectDither(x + 1, y + 1, w - 2, h - 2, Color::LightGray);
      renderer.drawRect(x + 1, y + 1, w - 2, h - 2, true);
    }
    // Only where the real boundary falls inside the mapped area: a band that
    // starts above it was clipped to area.y, and a line drawn there would claim
    // a division that is not where classifyZone() puts one.
    if (i > 0) {
      if (bands) {
        if (starts[i] > area.y && starts[i] < areaBottom) {
          renderer.drawLine(area.x, starts[i], area.x + area.width - 1, starts[i], true);
        }
      } else {
        renderer.drawLine(x, area.y, x, areaBottom - 1, true);
      }
    }

    const char* label = ReaderCombos::buttonLabel(kZoneBits[i]);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, x + (w - textW) / 2, y + (h - lineHeight) / 2, label);
  }
}
#endif

void ReaderComboWizardActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  char comboText[COMBO_TEXT_LEN];
  ReaderCombos::describe(captureMask, comboText, sizeof(comboText));

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CUSTOM_COMBO));

  const int topOffset = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - topOffset - metrics.buttonHintsHeight - metrics.verticalSpacing;

#if FREEINK_DEVICE_X4PRO
  // The chord is held against the glass here, so the content area IS the thing
  // being configured: the zone map takes it, and what has been captured so far
  // moves up beside the prompt.
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    message ? message : kPrompt(), captureMask == 0 ? tr(STR_NOT_SET) : comboText);
  drawZoneMap(Rect{0, topOffset, pageWidth, contentHeight});
#else
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    kPrompt());
  GUI.drawList(
      renderer, Rect{0, topOffset, pageWidth, contentHeight}, kStepCount, static_cast<int>(step),
      [](int index) -> std::string { return index == kStepAction ? tr(STR_COMBO_ACTION) : tr(STR_COMBO_BUTTONS); },
      nullptr, nullptr,
      [&](int index) -> std::string {
        if (index == kStepButtons) return captureMask == 0 ? tr(STR_NOT_SET) : comboText;
        return tr(STR_NOT_SET);
      },
      true);

  // Help lines sit below the two rows rather than at a fixed height, so they
  // follow the theme's row metrics instead of assuming a screen size.
  const int helpTop = topOffset + kStepCount * metrics.listRowHeight + 2 * metrics.verticalSpacing;
  GUI.drawHelpText(renderer, Rect{0, helpTop, pageWidth, 20}, kCancelHint());
  if (message) {
    GUI.drawHelpText(renderer, Rect{0, helpTop + 20 + metrics.verticalSpacing, pageWidth, 20}, message);
  }
#endif

#if FREEINK_DEVICE_X4PRO
  // The one slot this screen can honestly offer, drawn in the board's normal
  // lone-Back shape. Back is not capturable here, unlike on the X3/X4 where
  // every front button is a capture target and labelling them would be a lie,
  // so there is no conflict in showing it. heldTouchZone() excludes whatever
  // the bar publishes, so tapping it cannot also land in the chord.
  GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");
#endif
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
