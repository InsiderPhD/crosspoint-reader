#pragma once

#include <HalGPIO.h>

class GfxRenderer;

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

#if FREEINK_DEVICE_X4PRO
  // Screen tap zones: logical-frame thirds of the screen width.
  enum class TapZone : uint8_t { None, Left, Middle, Right };

  // Orientation source for swipe direction classification. Set once in setup();
  // a setter (not a constructor arg) because the global MappedInputManager is
  // constructed before the renderer's orientation is meaningful.
  void setRenderer(const GfxRenderer* r) { renderer = r; }

  // When true (everywhere except the readers), a home-key tap injects Confirm.
  // The readers disable this and dispatch their own configurable actions.
  void setHomeKeyActsAsConfirm(bool enabled) { homeKeyActsAsConfirm = enabled; }

  // When true, a completed screen tap anywhere injects Confirm. The main loop
  // disables this for activities that consume touch themselves (readers'
  // tap zones, keyboard hit-testing) via Activity::consumesTouchInput().
  void setTapActsAsConfirm(bool enabled) { tapActsAsConfirm = enabled; }

  // When true (Full Touch mode outside the readers), swipe synthesis drops
  // everything except the Back swipe: taps do the selecting/activating there,
  // and a sloppy tap classified as a swipe must not act invisibly. The main
  // loop keeps this false inside reader activities so their configured swipe
  // actions keep working unchanged. Up/down/right swipes are not injected but
  // are recorded for wasSwipe() so screens can give them a meaning of their own.
  void setSwipesBackOnly(bool enabled) { swipesBackOnly = enabled; }

  // Swipe recorded this frame, and only while swipesBackOnly is on (i.e. Full
  // Touch mode outside the reading screens). Nothing was injected for it, so a
  // consumer is free to give it a meaning of its own: paginated lists turn
  // Up/Down into a whole-page jump (TouchListNav::pageSwipe) and the tabbed
  // screens turn Right into "next tab" (TouchListNav::tabSwipeNext).
  //
  // Left is deliberately absent: it is still INJECTED as Back, because it is
  // the only Back the X4 Pro has (no front buttons). A screen that wants the
  // left swipe reads the Back press instead — see SettingsActivity's Back
  // handler, which steps to the previous tab there rather than closing.
  // Cleared at the start of every update().
  enum class Swipe : uint8_t { None, Up, Down, Right };
  Swipe wasSwipe() const { return swipe; }

  // Completed screen tap this frame, classified into left/middle/right thirds
  // of the logical screen. None while a home-key event fires (a bar contact
  // also reports as an edge tap and must not double-dispatch).
  TapZone wasTapZone() const;

  // Completed screen tap this frame in logical screen coordinates for the
  // active orientation, i.e. the same frame the activity draws in. For
  // consumers that hit-test real UI geometry (the keyboard); wasTapZone() is
  // the coarse thirds classification the readers use. Same home-key exclusion.
  bool wasTapPoint(int& lx, int& ly) const;

  // Finger down and stationary past the SDK long-press threshold, in the same
  // coordinate space as wasTapPoint(). Fires once per contact, while the finger
  // is still down — the lift afterwards still reports as a tap, so a consumer
  // that acts on this MUST call suppressTouchContact() or the same key
  // double-dispatches.
  bool wasTouchLongPressPoint(int& lx, int& ly) const;

  // Long-press analogue of wasTapZone(): the same event as
  // wasTouchLongPressPoint(), classified into the same left/middle/right
  // thirds. The point out-params let a consumer that needs the exact spot
  // (clip selection) keep it. Same suppressTouchContact() obligation.
  TapZone wasTouchLongPressZone(int& lx, int& ly) const;

  // Drop the remainder of the current contact after consuming a gesture.
  void suppressTouchContact() const { gpio.suppressTouchContact(); }

  // Home-key events, for the readers' configurable actions.
  bool wasHomeKeyTapped() const { return gpio.wasHomeKeyTapped(); }
  bool wasHomeKeyLongPressed() const { return gpio.wasHomeKeyLongPressed(); }
#endif

  void update() const {
    gpio.update();
#if FREEINK_DEVICE_X4PRO
    processTouchInput();
#endif
  }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getHeldTime(Button button) const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;
  // Resolves a logical button to the physical hardware index (used for BLE virtual injection).
  uint8_t getPhysicalButtonIndex(Button button) const;

 private:
  HalGPIO& gpio;

#if FREEINK_DEVICE_X4PRO
  const GfxRenderer* renderer = nullptr;
  bool homeKeyActsAsConfirm = true;
  bool tapActsAsConfirm = true;
  bool swipesBackOnly = false;
  // Written by the const processTouchInput(); the injected presses live in gpio
  // so this is the only piece of per-frame touch state the manager itself owns.
  mutable Swipe swipe = Swipe::None;

  // A touch point mapped into the logical frame, carried with the logical
  // screen size so callers can classify against it without re-deriving it.
  struct LogicalTouchPoint {
    int x;
    int y;
    int width;
    int height;
  };

  // Normalised GT911 point (native panel frame) -> logical screen point.
  LogicalTouchPoint toLogicalPoint(float nx, float ny) const;

  // Polls the SDK swipe/home-key detectors and injects the matching synthesized
  // button presses (see the X4 Pro mapping table in MappedInputManager.cpp).
  void processTouchInput() const;
#endif

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
