#include "HeaderDateUtils.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

namespace {
// Formats the date as "dd/mm/yyyy" with a leading "?" when the source is a stale
// fallback (no fresh NTP/manual-date this boot). The "?" is the user-visible cue
// that any reading recorded right now will be attributed to a date the device
// can't actually verify is correct — see the step-4 date-correctness plan.
std::string formatHeaderDateText(const uint32_t timestamp, const bool usedFallback) {
  const std::string body = TimeUtils::formatDate(timestamp, false);
  if (body.empty()) {
    return "";
  }
  return usedFallback ? ("?" + body) : body;
}
}  // namespace

HeaderDateUtils::DisplayDateInfo HeaderDateUtils::getDisplayDateInfo() {
  TimeUtils::configureTimezone();
  const uint32_t authoritativeTimestamp = TimeUtils::getAuthoritativeTimestamp();
  if (TimeUtils::isClockValid(authoritativeTimestamp)) {
    return {authoritativeTimestamp, false};
  }

  if (TimeUtils::isClockValid(APP_STATE.lastKnownValidTimestamp)) {
    return {APP_STATE.lastKnownValidTimestamp, true};
  }

  bool usedFallback = false;
  const uint32_t timestamp = READING_STATS.getDisplayTimestamp(&usedFallback);
  return {timestamp, usedFallback};
}

std::string HeaderDateUtils::getDisplayDateText() {
  const auto info = getDisplayDateInfo();
  return formatHeaderDateText(info.timestamp, info.usedFallback);
}

void HeaderDateUtils::drawTopLine(GfxRenderer& renderer) {
  const std::string dateText = getDisplayDateText();
  if (dateText.empty()) {
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Draw on the LEFT, vertically aligned with battery-percentage on the right.
  // The "+5" matches the y-offset the themes use for battery text.
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 5, dateText.c_str());
}

void HeaderDateUtils::drawHeaderWithDate(GfxRenderer& renderer, const char* title, const char* subtitle) {
  // Thin wrapper kept for callers that want a one-call header+date. drawHeader
  // already overlays the date itself (both BaseTheme and LyraTheme call
  // drawTopLine internally), so no second pass is needed here.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title, subtitle);
}
