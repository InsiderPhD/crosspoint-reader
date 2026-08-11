#include "MappedInputManager.h"

#include "CrossPointSettings.h"

#if FREEINK_DEVICE_X4PRO
#include <GfxRenderer.h>
#include <Logging.h>
#endif

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};
}  // namespace

uint8_t MappedInputManager::getPhysicalButtonIndex(const Button button) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

#if FREEINK_DEVICE_X4PRO
  // X4 Pro has no front buttons. The four front indices are free and are
  // synthesized from touch swipes by processTouchInput(); the two physical side
  // keys (SDK BTN_UP = left key, BTN_DOWN = right key) act as logical
  // Left/Right in menus while keeping their PageBack/PageForward role in the
  // readers. SETTINGS.frontButton* is deliberately bypassed so imported or
  // stale remap settings can never make Back/Confirm unreachable.
  //
  //   swipe left  -> BTN_BACK    -> Back
  //   swipe right -> BTN_CONFIRM -> Confirm
  //   swipe up    -> BTN_LEFT    -> Up
  //   swipe down  -> BTN_RIGHT   -> Down
  //   left key    -> BTN_UP      -> Left  (+ PageBack via kSideLayouts)
  //   right key   -> BTN_DOWN    -> Right (+ PageForward via kSideLayouts)
  switch (button) {
    case Button::Back:
      return HalGPIO::BTN_BACK;
    case Button::Confirm:
      return HalGPIO::BTN_CONFIRM;
    case Button::Up:
      return HalGPIO::BTN_LEFT;
    case Button::Down:
      return HalGPIO::BTN_RIGHT;
    case Button::Left:
      return HalGPIO::BTN_UP;
    case Button::Right:
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      return side.pageBack;
    case Button::PageForward:
      return side.pageForward;
  }
#else
  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return SETTINGS.frontButtonBack;
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return SETTINGS.frontButtonConfirm;
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return SETTINGS.frontButtonLeft;
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return SETTINGS.frontButtonRight;
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return HalGPIO::BTN_UP;
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      // Power button bypasses remapping.
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return side.pageBack;
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return side.pageForward;
  }
#endif

  return HalGPIO::BTN_BACK;
}

#if FREEINK_DEVICE_X4PRO
void MappedInputManager::processTouchInput() const {
  // Home key first: a bar contact also reports as an edge screen tap, so drop
  // the rest of the contact the moment a home-key event fires (the GT911 raises
  // it before the finger lifts) — otherwise the lift would double-dispatch as a
  // screen tap. Outside the readers a tap means Confirm; the readers poll
  // wasHomeKeyTapped()/wasHomeKeyLongPressed() themselves for their
  // configurable actions.
  if (gpio.wasHomeKeyTapped() || gpio.wasHomeKeyLongPressed()) {
    gpio.suppressTouchContact();
    if (homeKeyActsAsConfirm && gpio.wasHomeKeyTapped() && !gpio.isPressed(HalGPIO::BTN_CONFIRM)) {
      gpio.injectButtonPress(HalGPIO::BTN_CONFIRM);
    }
    return;
  }

  float nxs, nys, nxe, nye;
  if (!gpio.wasSwipe(nxs, nys, nxe, nye)) {
    // No swipe this frame: in plain UIs a completed tap anywhere is Confirm.
    // Activities that consume touch themselves (readers, keyboard) disable
    // this via setTapActsAsConfirm(false), driven from the main loop.
    float ntx, nty;
    if (tapActsAsConfirm && gpio.wasTouchTap(ntx, nty) && !gpio.isPressed(HalGPIO::BTN_CONFIRM)) {
      gpio.injectButtonPress(HalGPIO::BTN_CONFIRM);
    }
    return;
  }

  // Deltas in calibrated panel pixels. The GT911's calibrated frame (post
  // swapXY/flipY) has its x axis along the panel's 800px dimension and y along
  // the 480px dimension, but with y INVERTED relative to the renderer's
  // physical frame — established empirically from directed swipes on hardware
  // (2026-08-11 capture): in Portrait, a left flick gives a large negative
  // y delta, a right flick positive y, an up flick negative x, down positive x.
  const int dcx = static_cast<int>((nxe - nxs) * 800.0f);
  const int dcy = static_cast<int>((nye - nys) * 480.0f);
  const int pdx = dcx;
  const int pdy = -dcy;

  // Rotate physical deltas into the logical frame of the active orientation
  // (inverse of GfxRenderer::rotateCoordinates) so a swipe tracks what is on
  // screen. Falls back to the native landscape frame before setRenderer().
  int ldx = pdx;
  int ldy = pdy;
  if (renderer != nullptr) {
    switch (renderer->getOrientation()) {
      case GfxRenderer::Portrait:
        ldx = -pdy;
        ldy = pdx;
        break;
      case GfxRenderer::PortraitInverted:
        ldx = pdy;
        ldy = -pdx;
        break;
      case GfxRenderer::LandscapeClockwise:
        ldx = -pdx;
        ldy = -pdy;
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        break;
    }
  }

  uint8_t idx;
  if (abs(ldx) >= abs(ldy)) {
    idx = ldx < 0 ? HalGPIO::BTN_BACK : HalGPIO::BTN_CONFIRM;  // left / right
  } else {
    idx = ldy < 0 ? HalGPIO::BTN_LEFT : HalGPIO::BTN_RIGHT;  // up / down
  }
  LOG_DBG("INPUT", "Swipe cal=(%d,%d) logical=(%d,%d) -> btn %u", dcx, dcy, ldx, ldy, idx);

  // A held BLE remote button on the same index must not be disturbed: the
  // injected release half would clear the remote's hold.
  if (gpio.isPressed(idx)) {
    return;
  }
  gpio.injectButtonPress(idx);
}

MappedInputManager::LogicalTouchPoint MappedInputManager::toLogicalPoint(const float nx, const float ny) const {
  // Calibrated point -> physical panel point, both axes direct.
  //
  // Deliberately NOT the y-inverted frame processTouchInput() uses for swipe
  // deltas. That inversion here mirrored the logical x axis in portrait: a tap
  // on the leftmost key of a keyboard row landed on the rightmost key of the
  // same row (verified on hardware, portrait). Dropping it makes this the plain
  // inverse of GfxRenderer::rotateCoordinates with no correction term.
  //
  // The two paths therefore disagree about the panel's y direction, and only
  // one of them can match the hardware. The swipe mapping is left alone because
  // its gesture-to-button assignment is what users have; if it is ever
  // recalibrated, unify it here rather than reintroducing a flip on this path.
  //
  // Falls back to the native landscape frame before setRenderer(), which is
  // also the only case the panel size is not known.
  const int panelW = renderer != nullptr ? renderer->getDisplayWidth() : 800;
  const int panelH = renderer != nullptr ? renderer->getDisplayHeight() : 480;
  const int px = static_cast<int>(nx * static_cast<float>(panelW));
  const int py = static_cast<int>(ny * static_cast<float>(panelH));

  // Physical -> logical for the active orientation (inverse of
  // GfxRenderer::rotateCoordinates), so the result is in the same frame the
  // activity drew in.
  LogicalTouchPoint p{px, py, panelW, panelH};
  if (renderer != nullptr) {
    switch (renderer->getOrientation()) {
      case GfxRenderer::Portrait:
        p = {panelH - 1 - py, px, panelH, panelW};
        break;
      case GfxRenderer::PortraitInverted:
        p = {py, panelW - 1 - px, panelH, panelW};
        break;
      case GfxRenderer::LandscapeClockwise:
        p = {panelW - 1 - px, panelH - 1 - py, panelW, panelH};
        break;
      case GfxRenderer::LandscapeCounterClockwise:
        break;
    }
  }
  return p;
}

bool MappedInputManager::wasTapPoint(int& lx, int& ly) const {
  // A frame with a home-key event is a bar contact, never a screen tap.
  if (gpio.wasHomeKeyTapped() || gpio.wasHomeKeyLongPressed()) {
    return false;
  }
  float nx, ny;
  if (!gpio.wasTouchTap(nx, ny)) {
    return false;
  }
  const LogicalTouchPoint p = toLogicalPoint(nx, ny);
  lx = p.x;
  ly = p.y;
  return true;
}

bool MappedInputManager::wasTouchLongPressPoint(int& lx, int& ly) const {
  if (gpio.wasHomeKeyTapped() || gpio.wasHomeKeyLongPressed()) {
    return false;
  }
  float nx, ny;
  if (!gpio.wasTouchLongPress(nx, ny)) {
    return false;
  }
  const LogicalTouchPoint p = toLogicalPoint(nx, ny);
  lx = p.x;
  ly = p.y;
  return true;
}

MappedInputManager::TapZone MappedInputManager::wasTapZone() const {
  // A frame with a home-key event is a bar contact, never a screen tap.
  if (gpio.wasHomeKeyTapped() || gpio.wasHomeKeyLongPressed()) {
    return TapZone::None;
  }
  float nx, ny;
  if (!gpio.wasTouchTap(nx, ny)) {
    return TapZone::None;
  }

  // Only the x coordinate matters for left/middle/right thirds.
  const LogicalTouchPoint p = toLogicalPoint(nx, ny);
  const TapZone zone = (p.x < p.width / 3)        ? TapZone::Left
                       : (p.x >= 2 * p.width / 3) ? TapZone::Right
                                                  : TapZone::Middle;
  LOG_DBG("INPUT", "Tap logical=(%d,%d)/%d -> zone %u", p.x, p.y, p.width, static_cast<unsigned>(zone));
  return zone;
}
#endif

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  return (gpio.*fn)(getPhysicalButtonIndex(button));
}

bool MappedInputManager::wasPressed(const Button button) const { return mapButton(button, &HalGPIO::wasPressed); }

bool MappedInputManager::wasReleased(const Button button) const { return mapButton(button, &HalGPIO::wasReleased); }

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const { return gpio.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

unsigned long MappedInputManager::getHeldTime(const Button button) const {
  return gpio.getHeldTime(getPhysicalButtonIndex(button));
}

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Swap previous/next labels to match the page turn direction swap in INVERTED and LANDSCAPE_CCW.
  const bool swapLabels =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
  const char* leftLabel = swapLabels ? next : previous;
  const char* rightLabel = swapLabels ? previous : next;

  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return leftLabel;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return rightLabel;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}
