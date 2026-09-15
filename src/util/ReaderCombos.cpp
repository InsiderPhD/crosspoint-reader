#include "ReaderCombos.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"

namespace {
using S = CrossPointSettings;

// Bit position -> logical button, for the bits that ARE buttons. Index order IS
// the persisted bit order, so this table must never be reordered (see
// CrossPointSettings::COMBO_BUTTON). The zone bits have no entry: they come
// from the touch layer, not from a button.
constexpr MappedInputManager::Button kComboButtons[] = {
    MappedInputManager::Button::Back,  MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
    MappedInputManager::Button::Right, MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
    MappedInputManager::Button::Power,
};
constexpr uint8_t kComboButtonBits = sizeof(kComboButtons) / sizeof(kComboButtons[0]);
static_assert(kComboButtonBits == S::COMBO_BTN_ZONE_LEFT, "button bits must be the first COMBO_BTN_* values");

#if FREEINK_DEVICE_X4PRO
// Zone names follow the axis in force, exactly as the Reader Controls rows do:
// under TAP_ZONES_TOP_BOTTOM the Left slot IS the top band.
bool tapZonesAreBands() { return SETTINGS.readerTapZoneLayout == CrossPointSettings::TAP_ZONES_TOP_BOTTOM; }
#endif
}  // namespace

uint16_t ReaderCombos::currentMask(const MappedInputManager& input) {
  uint16_t mask = 0;
  for (uint8_t bit = 0; bit < kComboButtonBits; bit++) {
    if (input.isPressed(kComboButtons[bit])) mask |= static_cast<uint16_t>(1u << bit);
  }
#if FREEINK_DEVICE_X4PRO
  if (input.isHomeKeyDown()) mask |= 1u << S::COMBO_BTN_HOME;
  // A finger resting in a zone is held state, the same as a key being down.
  switch (input.heldTouchZone()) {
    case MappedInputManager::TapZone::Left:
      mask |= 1u << S::COMBO_BTN_ZONE_LEFT;
      break;
    case MappedInputManager::TapZone::Middle:
      mask |= 1u << S::COMBO_BTN_ZONE_MIDDLE;
      break;
    case MappedInputManager::TapZone::Right:
      mask |= 1u << S::COMBO_BTN_ZONE_RIGHT;
      break;
    case MappedInputManager::TapZone::None:
      break;
  }
#endif
  // Bits this board cannot hold are dropped rather than matched: on the X4 Pro
  // the front four are swipes, and a swipe's synthesized press lasts a frame —
  // long enough to pollute a grip that is being captured or matched.
  return mask & eligibleMask();
}

const char* ReaderCombos::buttonLabel(const uint8_t bit) {
  switch (bit) {
    case S::COMBO_BTN_BACK:
      return tr(STR_BACK);
    case S::COMBO_BTN_CONFIRM:
      return tr(STR_CONFIRM);
    case S::COMBO_BTN_LEFT:
      return tr(STR_DIR_LEFT);
    case S::COMBO_BTN_RIGHT:
      return tr(STR_DIR_RIGHT);
    case S::COMBO_BTN_SIDE_UP:
      // The X3's side keys sit left and right of the screen, not above and
      // below — same wording rule as the Reader Controls rows.
      return gpio.deviceIsX3() ? tr(STR_DIR_SIDE_L) : tr(STR_DIR_UP);
    case S::COMBO_BTN_SIDE_DOWN:
      return gpio.deviceIsX3() ? tr(STR_DIR_SIDE_R) : tr(STR_DIR_DOWN);
    case S::COMBO_BTN_POWER:
      // Untranslated, matching the Reader Controls row for the same key.
      return "Power";
#if FREEINK_DEVICE_X4PRO
    case S::COMBO_BTN_HOME:
      return tr(STR_HOME_BUTTON);
    // "Hold left/middle/right", because in a chord the finger stays down —
    // these are the same three thirds the tap rows name.
    case S::COMBO_BTN_ZONE_LEFT:
      return tapZonesAreBands() ? tr(STR_HOLD_TOP) : tr(STR_HOLD_LEFT);
    case S::COMBO_BTN_ZONE_MIDDLE:
      return tr(STR_HOLD_MIDDLE);
    case S::COMBO_BTN_ZONE_RIGHT:
      return tapZonesAreBands() ? tr(STR_HOLD_BOTTOM) : tr(STR_HOLD_RIGHT);
#endif
    default:
      return "";
  }
}

void ReaderCombos::describe(const uint16_t mask, char* out, const size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t bit = 0; bit < S::COMBO_BUTTON_COUNT && used + 1 < outLen; bit++) {
    if ((mask & (1u << bit)) == 0) continue;
    const int written = snprintf(out + used, outLen - used, used == 0 ? "%s" : " + %s", buttonLabel(bit));
    if (written <= 0) break;
    used += static_cast<size_t>(written);
    // Truncated: snprintf has already NUL-terminated what fitted, and `used`
    // is now past the buffer, so nothing may index with it again.
    if (used >= outLen) break;
  }
}

bool ReaderCombos::isPartialCombo(const uint16_t mask) {
  for (uint8_t slot = 0; slot < S::READER_COMBO_SLOTS; slot++) {
    const uint16_t combo = SETTINGS.readerComboButtons[slot];
    if (combo != 0 && (combo & mask) == mask) return true;
  }
  return false;
}

bool ReaderCombos::userComboHeld(const MappedInputManager& input) {
  const uint16_t mask = currentMask(input);
  return buttonCount(mask) >= 2 && slotForMask(mask) >= 0;
}

bool ReaderCombos::powerComboHeld(const MappedInputManager& input) {
  const uint16_t mask = currentMask(input);
  if ((mask & (1u << S::COMBO_BTN_POWER)) == 0) return false;
  return isPartialCombo(mask);
}

int8_t ReaderCombos::slotForMask(const uint16_t mask, const int8_t exceptSlot) {
  if (mask == 0) return -1;
  for (int8_t slot = 0; slot < static_cast<int8_t>(S::READER_COMBO_SLOTS); slot++) {
    if (slot == exceptSlot) continue;
    if (SETTINGS.readerComboButtons[slot] == mask) return slot;
  }
  return -1;
}

void ReaderCombos::reset() {
  lastMask = 0;
  maskChangedAt = 0;
  consuming = false;
  fired = S::READER_ACTION_NONE;
}

ReaderCombos::Result ReaderCombos::update(const MappedInputManager& input) {
  const uint16_t mask = currentMask(input);

  if (consuming) {
    // The frame where the last input comes up must be swallowed as well:
    // isPressed() falls and wasReleased() rises in the SAME frame, so handing
    // it back to the per-button dispatch would fire the chord's own keys as
    // short presses — the exact thing the chord is meant to replace.
    if (mask == 0) consuming = false;
    return Result::Consumed;
  }

  if (mask != lastMask) {
    lastMask = mask;
    maskChangedAt = millis();
  }

  if (buttonCount(mask) < 2) return Result::None;
  // Two or more inputs that could still grow into a chord. Hold the frame back
  // while the grip settles: a side key whose long press is None dispatches on
  // PRESS, so without this the second key of the chord would turn a page on
  // its way into the combo.
  if (!isPartialCombo(mask)) return Result::None;
  if (millis() - maskChangedAt < SETTLE_MS) return Result::Consumed;

  const int8_t slot = slotForMask(mask);
  // Settled on a grip that is only part of a longer combo: give the inputs back
  // to the normal dispatch rather than swallowing them for as long as they are
  // held.
  if (slot < 0) return Result::None;
  const uint8_t action = SETTINGS.readerComboAction[slot];
  // Sanitising on load already clears a slot bound to nothing, so this is a
  // belt-and-braces guard, not a path the UI can produce.
  if (action == S::READER_ACTION_NONE) return Result::None;

#if FREEINK_DEVICE_X4PRO
  if (mask & zoneMask()) {
    // The finger is still down. Spend the rest of the contact here, or lifting
    // it reports a tap and runs that zone's own action on top of the chord's.
    // Suppression also clears the zone bit immediately, which is why
    // `consuming` waits on mask == 0 rather than on the zone alone.
    input.suppressTouchContact();
  }
#endif

  fired = action;
  consuming = true;
  return Result::Fired;
}
