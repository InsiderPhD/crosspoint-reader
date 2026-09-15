#pragma once

#include <cstdint>

#include "activities/Activity.h"

struct Rect;

/**
 * Two-step wizard that defines one custom combo (chord).
 *
 * Step 1 records the inputs: the user holds them together and lets go, and the
 * union of everything that was down during that grip is the combo. Step 2 is
 * the shared reader-action picker.
 *
 * Nothing is persisted here — the chosen chord and action come back as a
 * ReaderComboResult and ReaderComboListActivity writes the slot, so the whole
 * screen stays one batched save (SPIFFS write throttling, see CLAUDE.md).
 *
 * Cancelling has to dodge the capture itself, and the two boards do it
 * differently. On the X3/X4 every front button is a capture target, so the way
 * out is Back on its own — a one-input "chord" is not one anyway. The X4 Pro
 * has no front buttons and its Back is a swipe (never capturable there), so the
 * way out is the capacitive home key, which is not part of any chord.
 *
 * The capture step deliberately draws NO button hints: on the X4 Pro that is
 * what keeps the Full Touch action bar off this screen, and the bar would
 * otherwise eat the taps this screen exists to record.
 */
class ReaderComboWizardActivity final : public Activity {
 public:
  ReaderComboWizardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint8_t slot);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Reads raw touch itself, so the main loop must not turn a tap or a home-key
  // press into a synthetic Confirm while a chord is being recorded.
  bool consumesTouchInput() const override { return true; }

 private:
  enum class Step : uint8_t { Capture, PickAction };

#if FREEINK_DEVICE_X4PRO
  // Paints the three tap zones across `area`, so a board whose chords are held
  // against the glass shows WHERE to hold. The dividers are drawn at the real
  // classification boundaries (thirds of the whole logical frame, along the
  // axis readerTapZoneLayout picks), not at thirds of `area` — a map that did
  // not match the hit test would be worse than no map. `area` stops above the
  // action bar, which is the Back button and is excluded from capture.
  void drawZoneMap(const Rect& area) const;
#endif

  void openActionPicker();
  void restartCapture(const char* message);
  void cancel();

  // Slot being edited. Kept so a chord identical to the one already in THIS
  // slot is not rejected as a duplicate of itself.
  const uint8_t slot;

  Step step = Step::Capture;
  // Union of the combo-eligible buttons held during the current grip.
  uint16_t captureMask = 0;
  // True once a key has gone down in this grip, so the all-keys-up frame can be
  // told apart from the idle state before the user starts.
  bool gripStarted = false;
  // Nothing is read until every button has been released once. Entering the
  // wizard is itself a Confirm press, and the picker returns on a Back press —
  // without this, the key that opened the screen joins the chord.
  bool armed = false;

  // Transient banner ("hold at least two", "already used"), same pattern as
  // ButtonRemapActivity.
  const char* message = nullptr;
  unsigned long messageUntil = 0;
};
