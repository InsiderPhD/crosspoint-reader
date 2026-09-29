#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <FreeInkUIGfxRenderer.h>

#include "UITheme.h"

// Bridges the active UITheme into FreeInkUI theme tokens for the screens that
// render through FreeInkApp (the SD-plugin catalog).
//
// Upstream drives every shape token from ThemeMetrics, because its whole UI
// went through FreeInkUI and ThemeMetrics grew the matching fields. Here the
// themes still draw their own lists and headers, so ThemeMetrics carries no
// FreeInkUI shape data: the SDK's line-height-derived defaults supply the
// shape, and only the values our metrics genuinely own are overridden, so a
// fui screen sits in the same bands, with the same scroll indicator, as every
// GUI-drawn screen around it. Row height is not set here — UiListActivity
// overrides it per screen, since only the caller knows whether its rows carry
// a subtitle.
inline freeink::ui::ThemeTokens uiThemeTokens(const freeink::ui::GfxRendererTarget& target) {
  namespace fui = freeink::ui;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  fui::ThemeTokens tokens = fui::themeTokensForLineHeight(target.lineHeight(fui::GfxRendererTarget::FONT_BODY));
  // Screen::header()/status() band height. Without this the SDK's
  // line-height-derived default applies and a fui-drawn header comes out a
  // different height than every GUI.drawHeader band.
  tokens.headerHeight = static_cast<int16_t>(metrics.headerHeight);
  tokens.headerSidePadding = static_cast<int16_t>(metrics.contentSidePadding);
  tokens.listSidePadding = static_cast<int16_t>(metrics.contentSidePadding);
  tokens.listScrollWidth = static_cast<int16_t>(metrics.scrollBarWidth);
  // Inward offset of the scroll indicator from the band edge: our themes
  // already carry it as the right-edge offset their own drawList uses, and it
  // exists for the same reason (boards whose panel sits recessed behind the
  // bezel would otherwise hide the track).
  tokens.listScrollInset = static_cast<int16_t>(metrics.scrollBarRightOffset);
  return tokens;
}

#endif  // CROSSPOINT_SD_PLUGINS
