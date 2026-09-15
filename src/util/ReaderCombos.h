#pragma once

#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"

class MappedInputManager;

/**
 * Custom button combos (chords) for the readers.
 *
 * A combo is two or more inputs held together, bound to one reader action
 * (SETTINGS.readerComboButtons / readerComboAction, see CrossPointSettings).
 * This class is the whole runtime side: the readers call update() once per
 * input frame, before their per-button dispatch, and act on what it returns.
 *
 * Why it has to sit in front of that dispatch: a short press fires on RELEASE,
 * so a chord whose keys were not swallowed would run the combo action and then
 * every single-button binding underneath it as the fingers came off — one
 * press turning a page, syncing and opening the menu. update() therefore keeps
 * reporting Consumed until the last input of the chord is released.
 *
 * Buttons are LOGICAL roles (MappedInputManager::Button), so a combo follows
 * the user's front-button remap, but the two side keys are the physically
 * fixed Up/Down rather than PageBack/PageForward: a chord is a grip, and the
 * grip should not move when the page-turn swap is flipped.
 *
 * On the X4 Pro a chord member can also be a SCREEN ZONE — a finger resting in
 * one of the reader's three tap thirds — because that board's Back/Confirm/
 * Left/Right are swipes, which cannot be held at all. A held zone is real level
 * state (MappedInputManager::heldTouchZone()), so "hold the left third, press a
 * side key" is a grip in exactly the way a two-key chord is. Firing such a
 * chord suppresses the rest of the contact, or the finger coming off would
 * *also* run that zone's own tap action.
 *
 * Power is a chord member like any other. Its own claims on the key are handled
 * rather than avoided: the 500ms sleep hold yields via powerComboHeld(), and
 * the built-in POWER+Down / POWER+Confirm chords yield via userComboHeld() —
 * so those two can be rebound like any other grip, and the actions they ran
 * (Screenshot, RAM report) are pickable actions in their own right.
 *
 * No heap, no state beyond a few bytes and a timestamp.
 */
class ReaderCombos {
 public:
  enum class Result : uint8_t {
    None,      // nothing to do; run the normal per-button dispatch
    Consumed,  // a chord is forming or being released — ignore input this frame
    Fired,     // run firedAction(), then stop processing input this frame
  };

  // Call once per reader input frame, before any per-button handling.
  Result update(const MappedInputManager& input);

  // The action Fired refers to. Only meaningful in the frame Fired is returned.
  uint8_t firedAction() const { return fired; }

  // Forget any chord in progress. For a reader resuming after a sub-activity,
  // where the inputs were released on a screen this object never saw.
  void reset();

  // ── Shared with the settings UI ────────────────────────────────────────

  // Bitmask of the combo-eligible inputs currently held, in COMBO_BTN_* bit
  // order.
  static uint16_t currentMask(const MappedInputManager& input);

  // Bits this board can actually produce. The X4 Pro's front four are swipes
  // (nothing to hold), so there it is the two side keys, Power, the home key
  // and the three screen zones; everywhere else it is the seven buttons and
  // none of the touch bits. The settings FIELDS are identical on every build — only what can be
  // captured and matched differs, so a settings.json still round-trips between
  // boards.
  static constexpr uint16_t eligibleMask() {
    using S = CrossPointSettings;
#if FREEINK_DEVICE_X4PRO
    return (1u << S::COMBO_BTN_SIDE_UP) | (1u << S::COMBO_BTN_SIDE_DOWN) | (1u << S::COMBO_BTN_POWER) |
           (1u << S::COMBO_BTN_ZONE_LEFT) | (1u << S::COMBO_BTN_ZONE_MIDDLE) | (1u << S::COMBO_BTN_ZONE_RIGHT) |
           (1u << S::COMBO_BTN_HOME);
#else
    return (1u << S::COMBO_BTN_BACK) | (1u << S::COMBO_BTN_CONFIRM) | (1u << S::COMBO_BTN_LEFT) |
           (1u << S::COMBO_BTN_RIGHT) | (1u << S::COMBO_BTN_SIDE_UP) | (1u << S::COMBO_BTN_SIDE_DOWN) |
           (1u << S::COMBO_BTN_POWER);
#endif
  }

  // True when what is held right now IS a defined combo. main.cpp asks before
  // running its built-in POWER+Down / POWER+Confirm chords, and stands aside
  // inside a reader so a user who bound that grip gets their own action. It
  // does NOT stand aside elsewhere: those chords exist to take a reading on any
  // screen, including mid-render states no menu can reach, and a combo only
  // dispatches in a reader anyway.
  static bool userComboHeld(const MappedInputManager& input);

  // True while what is held is, or could still become, a defined combo that
  // includes Power. main.cpp asks before its 500ms sleep hold fires: the chord
  // itself has already run at SETTLE_MS, and the user is still holding the keys
  // down, which must not also put the device to sleep. Exact rather than
  // blanket — with no Power combo defined this is always false, so nothing
  // about the existing sleep behaviour changes.
  static bool powerComboHeld(const MappedInputManager& input);

  // Bits that are screen zones rather than keys. A chord holding any of these
  // owes the touch layer a suppressTouchContact() when it fires.
  static constexpr uint16_t zoneMask() {
    using S = CrossPointSettings;
    return (1u << S::COMBO_BTN_ZONE_LEFT) | (1u << S::COMBO_BTN_ZONE_MIDDLE) | (1u << S::COMBO_BTN_ZONE_RIGHT);
  }

  static uint8_t buttonCount(uint16_t mask) { return static_cast<uint8_t>(__builtin_popcount(mask)); }

  // Localized name of one COMBO_BTN_* bit, following the device's own wording:
  // SideL/SideR on the X3, Up/Down on the X4, and zone names that track
  // readerTapZoneLayout exactly as the Reader Controls rows do.
  static const char* buttonLabel(uint8_t bit);

  // "Back + Up" into a caller-owned buffer. A buffer rather than a shared
  // static so two descriptions can be alive at once (a list row and its value).
  static void describe(uint16_t mask, char* out, size_t outLen);

  // Slot holding exactly this mask, ignoring `exceptSlot`, or -1 for none.
  // Used by the wizard to refuse a duplicate before it is stored.
  static int8_t slotForMask(uint16_t mask, int8_t exceptSlot = -1);

 private:
  // A human pressing two inputs "together" lands them a few tens of ms apart,
  // so the mask grows for a moment before it settles. Firing on the first match
  // would make a three-input combo unreachable whenever its first two are
  // themselves a combo. Waiting for the mask to hold still costs nothing a
  // reader can feel.
  //
  // It does not rescue a chord assembled slowly: an input held past the
  // reader's long-press threshold (~500ms) fires its own long press, or its
  // zone's hold action, before the rest of the chord arrives. That is the deal
  // a chord makes — the inputs go down together — and the alternative (muting
  // every long press whose input appears in some combo) would cost far more
  // than it saves.
  static constexpr unsigned long SETTLE_MS = 60;

  // True when every held input belongs to one defined combo, i.e. this grip is
  // that chord or still on its way to being it.
  static bool isPartialCombo(uint16_t mask);

  uint16_t lastMask = 0;
  unsigned long maskChangedAt = 0;
  bool consuming = false;
  uint8_t fired = CrossPointSettings::READER_ACTION_NONE;
};
