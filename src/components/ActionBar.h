#pragma once

#include "MappedInputManager.h"

class GfxRenderer;

// --- X4 Pro Full Touch on-screen action bar ----------------------------------
//
// The X3/X4 label their four front buttons in a bottom hint bar, so every screen
// says how to leave it: "Back" sits under the button that goes back. The X4 Pro
// has no front buttons, so that bar was suppressed outright
// (buttonHintsHeight = 0, drawButtonHints() returning immediately) and screens
// were left with no visible way out at all — the BookFusion sync result screen
// drew "Cancel / Apply" into a zero-height strip and showed nothing, leaving the
// undiscoverable left-swipe as the only exit.
//
// This turns those labels back into something: real tap targets, drawn where a
// finger can reach them. Full Touch only — that is the mode where taps are the
// interaction model; the gesture mode keeps its reclaimed strip (see the two
// metrics tables in BaseTheme.h / LyraTheme.h).
//
// The bar IS this board's front button pair -- Back/Confirm here, and the outer
// two slots are logical Left/Right, exactly the roles the other boards put on
// front buttons. Up/Down are not here and never should be: those are the side
// keys, on this board as on the X3.
//
// Confirm drops out of that pair on the screens where a tap on the highlighted
// row or tile already runs it (Activity::tapActivatesConfirm) -- a plain menu
// then shows one full-width "Back", because "Select" there was only a second
// way to do what tapping the row does. Screens whose Confirm has no on-screen
// equivalent (the date spinners commit with it) keep their slot.
//
// By default only Back and Confirm get a slot, and they take the whole width:
// most screens use Left/Right for nothing but a second way to scroll, which the
// side keys already do, and two wide targets beat four narrow ones for a finger
// on e-ink. A screen whose Left/Right carry real ACTIONS (Libby's Renew/Return,
// WiFi's Forget/Retry) passes allSlots = true to drawButtonHints and gets all
// four. Four slots on a 480px panel come out at ~106px each, the same width the
// other boards give their four labels.
//
// The call sites are the themes' drawButtonHints() and the one screen that
// paints its own hint bar (ReadingStatsActivity::drawLyraStyleButtonHints), so
// no activity needs to opt in to having a bar at all -- only to how many slots
// it gets. Each paint publishes the slot rectangles it just drew and
// MappedInputManager::processTouchInput() routes a tap inside one of them to
// that slot's logical button. Paint and hit-test therefore cannot drift: nothing
// is hit-testable that was not just drawn, and ActivityManager clears the
// publication before every render so a screen that draws no hint bar leaves no
// ghost targets behind.
namespace ActionBar {

#if FREEINK_DEVICE_X4PRO

// Slot -> logical button, matching MappedInputManager::mapLabels' four
// arguments in order.
constexpr MappedInputManager::Button kSlotButtons[4] = {
    MappedInputManager::Button::Back,
    MappedInputManager::Button::Confirm,
    MappedInputManager::Button::Left,
    MappedInputManager::Button::Right,
};

// Draws whichever labels are non-empty as buttons, centred across the bottom
// strip and sharing its width, and publishes a tap target for each (slightly
// larger than the drawn box -- see the comment at the publish site). The theme
// blanks btn3/btn4 unless the screen opted into all four, so passing empty
// strings is the normal case rather than an edge one. barHeight comes from the
// theme metrics so the bar occupies exactly the strip the screen reserved for
// it; a barHeight of 0 (gesture mode) draws nothing.
void draw(GfxRenderer& renderer, int barHeight, int fontId, bool rounded, const char* btn1, const char* btn2,
          const char* btn3, const char* btn4);

// Logical button under a logical-frame point, or false on a miss.
bool hitTest(int lx, int ly, MappedInputManager::Button& outButton);

// Drop the published slots. Called before every render.
void clear();

// Whether the screen about to paint makes its Confirm slot redundant by having
// the drawn UI activate the same thing on a tap (Activity::tapActivatesConfirm).
// Published by ActivityManager before each render rather than passed through
// drawButtonHints: the answer belongs to the activity and is often state-
// dependent (a list screen with a modal open needs the slot back), and this way
// no screen has to thread a flag through its render path to get the right bar.
// A suppressed Confirm simply frees its width for the slots that remain.
void setConfirmRedundant(bool redundant);

#else
// Boards with real front buttons: the bar does not exist, so the call sites
// outside the themes (ActivityManager's pre-render reset) need no fence.
inline void clear() {}
inline void setConfirmRedundant(bool) {}
#endif

}  // namespace ActionBar
