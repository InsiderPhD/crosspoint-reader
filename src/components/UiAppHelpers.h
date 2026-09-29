#pragma once

#include <DevicePolicy.h>

#include <atomic>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"

// Shared glue for activities hosting a FreeInkApp: the font-bound render
// target and the touch snapshot FreeInkApp routing consumes.

// Theme tokens for a FreeInkUI screen.
//
// Upstream keeps one app-wide instance behind an atomic cell, because its whole
// UI is FreeInkUI and a per-app copy (~1.6KB) would be paid once per stacked
// activity. Two differences here make ownership the better trade: this fork has
// exactly one FreeInkUI screen (the plugin catalog), and this SDK's
// setThemeRef() takes the instance pointer rather than a cell — so a shared
// pool would sit in .data permanently (measured: 3376 bytes, a third of the
// smallest free-heap margin a section rebuild leaves) to serve a screen most
// sessions never open.
//
// So each UiAppHost owns its tokens: they exist only while that screen does,
// nothing is shared, and the torn-read hazard the atomic cell existed to
// prevent cannot arise — only the owning host's render task reads them, and
// they are rebuilt on screen entry before that task runs.

// Rebuild `tokens` from the active UITheme + this target's fonts and point the
// app at them. Called on screen entry (resetUi), so a theme or font change
// between activities is picked up.
//
// FreeInkApp::setThemeRef() takes a pointer to an atomic cell rather than to
// the tokens themselves, so a caller could swap instances under a running
// render task. Here the tokens are rebuilt on screen entry before that task
// runs, so the cell just points at this host's own instance.
template <typename App>
inline void applyUiTheme(App& app, freeink::ui::ThemeTokens& tokens,
                         std::atomic<const freeink::ui::ThemeTokens*>& tokensRef,
                         const freeink::ui::GfxRendererTarget& target) {
  tokens = uiThemeTokens(target);
  tokensRef.store(&tokens, std::memory_order_release);
  app.setThemeRef(&tokensRef);
}

// Bind the uiScale fonts before FreeInkApp's constructor derives its theme
// metrics from the body font's line height.
inline freeink::ui::GfxRendererTarget makeUiTarget(const GfxRenderer& renderer) {
  freeink::ui::GfxRendererTarget target(renderer);
  const auto spec = uiScaleSpec();
  target.setFont(freeink::ui::GfxRendererTarget::FONT_SMALL, spec.smallFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_BODY, spec.bodyFontId);
  target.setFont(freeink::ui::GfxRendererTarget::FONT_TITLE, spec.titleFontId);
  return target;
}

// Tap release with coords, plus the raw release the tap classifier never
// reports (swipe end, drag-off) delivered off-target: nothing dispatches,
// but routing drops its pressed-element state instead of ghosting it onto
// the next render.
// Upstream also carries listIconFor() here, mapping UIIcon -> a generated
// freeink::Icon for FreeInkUI list rows. It is omitted: this fork's icons are
// still the older raw-bitmap headers in components/icons/, which use a
// different bit layout, and no ported screen sets a row icon. Port
// components/icons/listIcons.h alongside it if a screen ever needs one.

// Bottom-anchored Cancel / OK pair for slider dialogs on touch devices, where
// the physical Back/Confirm buttons (and their auto-hidden hints) may not
// exist. Callers gate on hasTouch(): button boards keep the hint chrome and
// need no on-screen pair. Consumes the bottom of the screen's content band.
template <typename Screen>
inline void addDialogCancelOk(Screen& screen, const freeink::ui::ActionId cancelAction,
                              const freeink::ui::ActionId okAction) {
  const auto& theme = screen.theme();
  const int16_t sideInset = static_cast<int16_t>(theme.spaceLg * 2);
  const freeink::ui::Rect band =
      screen.takeBottom(theme.rowHeight, theme.spaceLg).inset(freeink::ui::Insets{0, sideInset, 0, sideInset});
  const int16_t gap = theme.spaceLg;
  const int16_t buttonWidth = static_cast<int16_t>((band.width - gap) / 2);

  freeink::ui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = cancelAction;
  cancel.inputMask = freeink::ui::InputTouch;
  cancel.text = theme.bodyText;
  freeink::ui::ButtonProps ok = cancel;
  ok.label = tr(STR_OK_BUTTON);
  ok.action = okAction;
  freeink::ui::button(screen.frame(), freeink::ui::Rect{band.x, band.y, buttonWidth, band.height}, cancel);
  freeink::ui::button(
      screen.frame(),
      freeink::ui::Rect{static_cast<int16_t>(band.x + band.width - buttonWidth), band.y, buttonWidth, band.height}, ok);
}

// withLongPress: the SDK touch classifier fires the long-press WHILE the
// finger is still down (matching the physical-button hold-to-act feel) and
// suppresses the remainder of the contact, so the finger lift can't also
// tap-dismiss the popup the long-press opens. Delivered as a touchReleased +
// longPress snapshot at the contact point; only rows masked InputLongPress
// receive it. Mirrors the SDK's long-press-aware fui::snapshotFrom, but maps
// coordinates through the renderer's LIVE orientation (the reader rotates at
// runtime), which the DeviceContext-based SDK adapter does not track.
inline freeink::ui::InputSnapshot touchSnapshotFrom(const MappedInputManager& mappedInput,
                                                    const bool withLongPress = false) {
  int tx = 0;
  int ty = 0;
  if (withLongPress && mappedInput.wasScreenLongPress(tx, ty)) {
    freeink::ui::InputSnapshot snap{};
    snap.touchReleased = true;
    snap.longPress = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
    return snap;
  }

  freeink::ui::InputSnapshot snap{};
  // Live contact position: only InputDrag-masked elements (sliders) react, so
  // carrying it in every snapshot is free for ordinary screens.
  if (mappedInput.isScreenTouchHeld(tx, ty)) {
    snap.touchHeld = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    snap.touchPressed = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    snap.touchReleased = true;
    snap.touchX = static_cast<int16_t>(tx);
    snap.touchY = static_cast<int16_t>(ty);
  } else if (mappedInput.wasScreenTouchReleased()) {
    snap.touchReleased = true;
    snap.touchX = -1;
    snap.touchY = -1;
  }
  return snap;
}

#endif  // CROSSPOINT_SD_PLUGINS
