#pragma once

#include <cstdint>

#include "MappedInputManager.h"

// Swallows the tail of a press that began on the screen BEFORE this one.
//
// A reader long-press fires while the key is still down, so when the action it
// runs opens a list (Select Chapter, ...) the release lands on the new screen:
// a Confirm release would pick the highlighted row and a side-key release (or
// its auto-repeat) would move the cursor. arm() in onEnter() records which keys
// are already down; consume() at the top of loop() then reports true -- skip
// this frame -- until every one of them has been let go. Keys pressed after the
// screen opened are untouched.
//
// One byte of state, no allocation; header-only so each screen that needs it
// pays only for the inlined loop.
class HeldButtonGuard {
 public:
  void arm(const MappedInputManager& input) {
    mask = 0;
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      if (input.isPressed(BUTTONS[i])) mask |= static_cast<uint8_t>(1u << i);
    }
  }

  bool consume(const MappedInputManager& input) {
    if (mask == 0) return false;
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      const uint8_t bit = static_cast<uint8_t>(1u << i);
      if ((mask & bit) && !input.isPressed(BUTTONS[i])) mask &= static_cast<uint8_t>(~bit);
    }
    // The frame carrying the last release is swallowed too: that release IS the
    // edge the screen would otherwise act on.
    return true;
  }

 private:
  // PageBack/PageForward resolve to the same physical side keys as Up/Down, so
  // listing Up/Down covers them.
  static constexpr MappedInputManager::Button BUTTONS[] = {
      MappedInputManager::Button::Back,  MappedInputManager::Button::Confirm, MappedInputManager::Button::Left,
      MappedInputManager::Button::Right, MappedInputManager::Button::Up,      MappedInputManager::Button::Down,
      MappedInputManager::Button::Power};
  static constexpr uint8_t BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
  uint8_t mask = 0;
};
