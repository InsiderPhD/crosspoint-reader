#include "HeaderDateUtils.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
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

bool HeaderDateUtils::formatDate(const DisplayDateInfo& info, char* out, const size_t outLen) {
  if (out == nullptr || outLen == 0) return false;
  const std::string text = formatHeaderDateText(info.timestamp, info.usedFallback);
  snprintf(out, outLen, "%s", text.c_str());
  return out[0] != '\0';
}

bool HeaderDateUtils::formatClock(const DisplayDateInfo& info, char* out, const size_t outLen) {
  if (out == nullptr || outLen == 0) return false;
  out[0] = '\0';
  if (info.usedFallback) return false;
  const std::string text = TimeUtils::formatTime(info.timestamp);
  snprintf(out, outLen, "%s", text.c_str());
  return out[0] != '\0';
}

void HeaderDateUtils::drawHeaderWithDate(GfxRenderer& renderer, const char* title, const char* subtitle) {
  // Thin wrapper kept for callers that want a one-call header+date. drawHeader
  // already draws the Outside Reader bar (date included) itself, so no second
  // pass is needed here.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title, subtitle);
}
