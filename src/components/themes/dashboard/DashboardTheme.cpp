#include "DashboardTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/afternoon.h"
#include "components/icons/book24.h"
#include "components/icons/cover.h"
#include "components/icons/evening.h"
#include "components/icons/morning.h"
#include "components/icons/night.h"
#include "components/icons/streak24.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

const ThemeMetrics& DashboardTheme::themeMetrics() const {
#if FREEINK_DEVICE_X4PRO
  // See BaseTheme::themeMetrics().
  return SETTINGS.fullTouchUi ? DashboardMetrics::values : DashboardMetrics::noActionBarValues;
#else
  return DashboardMetrics::values;
#endif
}

int DashboardTheme::hitTestRecentBookCover(const Rect& rect, const int slotCount, const int lx, const int ly) const {
  return BaseTheme::hitTestRecentBookCover(rect, slotCount, lx, ly);
}

namespace {

constexpr int kStatsFontId = UI_12_FONT_ID;
constexpr int kStatsLabelFontId = SMALL_FONT_ID;
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kFooterFontId = UI_10_FONT_ID;

constexpr int kContentInset = 20;
constexpr int kTopInset = 20;
constexpr int kCoverCornerRadius = 8;
constexpr int kStatsColumnWidth = 105;
constexpr int kStatsColumnWidthWide = 120;
constexpr int kCoverStatsGap = 15;
constexpr int kStatsValueLabelGap = 1;

constexpr int kTitleTopGap = 24;
constexpr int kTitleChapterGap = 6;
constexpr int kTitleMaxLines = 2;
constexpr int kChapterMaxLines = 2;

constexpr int kFooterIconSize = 24;
constexpr int kFooterIconTextGap = 14;
constexpr int kFooterBlockHeight = 40;
constexpr int kFooterMenuGap = 16;
constexpr int kFooterTextGap = 12;

constexpr int kMinCoverHeight = 140;
constexpr int kPlaceholderIconSize = 32;

// Wide means a landscape frame; the extra side inset keeps the pair from
// stranding the stats column against the far bezel.
bool isWideScreen(const GfxRenderer& renderer) { return renderer.getScreenWidth() >= 560; }

int statsColumnWidth(const GfxRenderer& renderer) {
  return isWideScreen(renderer) ? kStatsColumnWidthWide : kStatsColumnWidth;
}

// Side inset including the panel's physical bezel margin for this orientation,
// so the stats column never runs under the frame.
void contentInsets(const GfxRenderer& renderer, int& leftOut, int& rightOut) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  leftOut = kContentInset + left;
  rightOut = kContentInset + right;
}

struct Layout {
  Rect cover;
  int statsRightX;
  int textLeftX;
  int textWidth;
  // How many lines the title and chapter get. Both shrink on a short frame so
  // the block above the footer stays inside its budget rather than running
  // through it.
  int titleLines;
  int chapterLines;
  int footerCenterY;
  int footerLeftX;
  int footerRightX;
};

// Everything the draw helpers need, derived once. The cover box shrinks to fit
// whatever height is left between the header and the icon menu, so a short
// frame (landscape, or X4 Pro gesture mode) degrades instead of overflowing.
Layout computeLayout(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  int insetLeft = 0;
  int insetRight = 0;
  contentInsets(renderer, insetLeft, insetRight);

  const int contentTop = rect.y + kTopInset;
  const int menuTop = LyraCarouselTheme::menuBlockTopY(renderer, metrics);

  const int titleLineH = renderer.getLineHeight(kTitleFontId);
  const int footerBlockTop = menuTop - kFooterMenuGap - kFooterBlockHeight;

  // Give the cover whatever the title block leaves, then trade text lines away
  // until the cover clears its minimum. A landscape frame is barely half the
  // height of a portrait one, so four text lines simply do not fit there.
  const auto coverHeightFor = [&](const int titleLines, const int chapterLines) {
    const int textBlockH = titleLineH * (titleLines + chapterLines) + (chapterLines > 0 ? kTitleChapterGap : 0);
    return footerBlockTop - kFooterTextGap - textBlockH - kTitleTopGap - contentTop;
  };

  int titleLines = kTitleMaxLines;
  int chapterLines = kChapterMaxLines;
  while (titleLines + chapterLines > 1 && coverHeightFor(titleLines, chapterLines) < kMinCoverHeight) {
    if (chapterLines > 0) {
      chapterLines--;
    } else {
      titleLines--;
    }
  }

  const int coverH = std::clamp(coverHeightFor(titleLines, chapterLines), kMinCoverHeight,
                                std::min(DashboardMetrics::kMaxCoverHeight, metrics.homeCoverHeight));
  const int maxCoverW =
      renderer.getScreenWidth() - insetLeft - insetRight - statsColumnWidth(renderer) - kCoverStatsGap;
  const int coverW = std::max(1, std::min({DashboardMetrics::kMaxCoverWidth, (coverH * 2) / 3, maxCoverW}));

  Layout layout;
  layout.cover = Rect{insetLeft, contentTop, coverW, coverH};
  layout.statsRightX = renderer.getScreenWidth() - insetRight;
  layout.textLeftX = insetLeft;
  layout.textWidth = std::max(1, renderer.getScreenWidth() - insetLeft - insetRight);
  layout.titleLines = titleLines;
  layout.chapterLines = chapterLines;
  layout.footerCenterY = footerBlockTop + kFooterBlockHeight / 2;
  layout.footerLeftX = insetLeft;
  layout.footerRightX = renderer.getScreenWidth() - insetRight;
  return layout;
}

// ---- formatting ----

void formatDuration(const uint64_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const unsigned long hours = static_cast<unsigned long>(seconds / 3600u);
  const unsigned long minutes = static_cast<unsigned long>((seconds % 3600u) / 60u);
  if (hours == 0) {
    snprintf(buf, len, "%lu min", minutes);
  } else {
    snprintf(buf, len, "%luh %lu min", hours, minutes);
  }
}

void formatCompactDuration(const uint64_t seconds, char* buf, const size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const unsigned long minutes = static_cast<unsigned long>((seconds + 30u) / 60u);
  if (minutes < 60) {
    snprintf(buf, len, "%lu min", minutes);
    return;
  }
  const unsigned long hours = minutes / 60u;
  const unsigned long remainder = minutes % 60u;
  if (remainder == 0) {
    snprintf(buf, len, "%luh", hours);
  } else {
    snprintf(buf, len, "%luh %lum", hours, remainder);
  }
}

void formatShortDate(const uint32_t epochSeconds, char* buf, const size_t len) {
  const std::string formatted = TimeUtils::formatShortDate(epochSeconds);
  snprintf(buf, len, "%s", formatted.empty() ? "-" : formatted.c_str());
}

// ---- derived stats ----

uint64_t bookReadingSeconds(const ReadingBookStats* book) {
  return book != nullptr ? book->totalReadingMs / 1000u : 0u;
}

// Calendar days from the first read to the reference day, inclusive. 0 when
// either end is unknown.
uint32_t daysReading(const ReadingBookStats* book, const uint32_t referenceEpoch) {
  if (book == nullptr || book->firstReadAt == 0) {
    return 0;
  }
  const uint32_t endEpoch = (book->completed && book->completedAt != 0) ? book->completedAt : referenceEpoch;
  const uint32_t startDay = TimeUtils::getLocalDayOrdinal(book->firstReadAt);
  const uint32_t endDay = TimeUtils::getLocalDayOrdinal(endEpoch);
  if (startDay == 0 || endDay == 0 || endDay < startDay) {
    return 0;
  }
  return endDay - startDay + 1;
}

// Seconds of reading still to go, extrapolated from how long this book has
// taken to reach its current percentage. False below two minutes read, where
// the extrapolation is pure noise.
bool estimatedTimeLeft(const ReadingBookStats* book, const int progressPercent, uint64_t& secondsOut) {
  secondsOut = 0;
  const uint64_t readSeconds = bookReadingSeconds(book);
  if (progressPercent <= 0 || progressPercent >= 100 || readSeconds < 120) {
    return false;
  }
  const float progress = static_cast<float>(progressPercent) / 100.0f;
  const float estimate = (static_cast<float>(readSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }
  secondsOut = static_cast<uint64_t>(estimate + 0.5f);
  return secondsOut > 0;
}

// Projects the remaining reading time onto the calendar at the pace this book
// has actually been read at, so a book read 20 minutes a night lands weeks out
// rather than tomorrow. 0 when there is not enough history to scale by.
uint32_t estimatedFinishEpoch(const ReadingBookStats* book, const uint32_t referenceEpoch,
                              const uint64_t estimatedSeconds) {
  const uint64_t readSeconds = bookReadingSeconds(book);
  const uint32_t spanDays = daysReading(book, referenceEpoch);
  if (readSeconds == 0 || estimatedSeconds == 0 || spanDays == 0) {
    return 0;
  }
  const uint64_t calendarSeconds =
      (estimatedSeconds * static_cast<uint64_t>(spanDays) * 86400ull + readSeconds / 2ull) / readSeconds;
  return referenceEpoch + static_cast<uint32_t>(std::min<uint64_t>(calendarSeconds, UINT32_MAX - referenceEpoch));
}

// The reader's lifetime EMA, which is the only page-rate this fork tracks --
// there are no per-book page-turn counters to divide here.
float pagesPerMinute() {
  const uint16_t secondsPerPage = SETTINGS.readingSpeedSecondsPerPage;
  return secondsPerPage > 0 ? 60.0f / static_cast<float>(secondsPerPage) : 0.0f;
}

bool dominantTimeOfDayBucket(ReadingTimeBucket& bucketOut) {
  const uint64_t* buckets = READING_STATS.getTimeOfDayMs();
  size_t dominant = 0;
  uint64_t best = 0;
  for (size_t i = 0; i < READING_TIME_BUCKET_COUNT; i++) {
    if (buckets[i] > best) {
      best = buckets[i];
      dominant = i;
    }
  }
  if (best == 0) {
    return false;
  }
  bucketOut = static_cast<ReadingTimeBucket>(dominant);
  return true;
}

const char* readerTypeLabel() {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantTimeOfDayBucket(bucket)) {
    return tr(STR_STATS_NEW_READER);
  }
  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return tr(STR_STATS_MORNING_READER);
    case ReadingTimeBucket::Afternoon:
      return tr(STR_STATS_AFTERNOON_READER);
    case ReadingTimeBucket::Evening:
      return tr(STR_STATS_EVENING_READER);
    case ReadingTimeBucket::Night:
      break;
  }
  return tr(STR_STATS_NIGHT_READER);
}

const uint8_t* readerTypeIcon() {
  ReadingTimeBucket bucket = ReadingTimeBucket::Night;
  if (!dominantTimeOfDayBucket(bucket)) {
    return Book24Icon;
  }
  switch (bucket) {
    case ReadingTimeBucket::Morning:
      return MorningReaderIcon;
    case ReadingTimeBucket::Afternoon:
      return AfternoonReaderIcon;
    case ReadingTimeBucket::Evening:
      return EveningReaderIcon;
    case ReadingTimeBucket::Night:
      break;
  }
  return NightReaderIcon;
}

// ---- cover ----

Rect fittedBitmapRect(const Bitmap& bitmap, const Rect& target) {
  if (bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0 || target.width <= 0 || target.height <= 0) {
    return target;
  }
  // drawBitmap fits within and never enlarges, so mirror that here to know
  // where the artwork actually lands and frame it there.
  const float widthScale = static_cast<float>(target.width) / static_cast<float>(bitmap.getWidth());
  const float heightScale = static_cast<float>(target.height) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(1.0f, std::min(widthScale, heightScale));
  const int drawnW = std::clamp(static_cast<int>(bitmap.getWidth() * scale), 1, target.width);
  const int drawnH = std::clamp(static_cast<int>(bitmap.getHeight() * scale), 1, target.height);
  return Rect{target.x + (target.width - drawnW) / 2, target.y + (target.height - drawnH) / 2, drawnW, drawnH};
}

void drawPlaceholderCover(const GfxRenderer& renderer, const Rect& coverRect) {
  renderer.fillRoundedRect(coverRect.x, coverRect.y + coverRect.height / 3, coverRect.width, 2 * coverRect.height / 3,
                           kCoverCornerRadius, /*roundTopLeft=*/false, /*roundTopRight=*/false,
                           /*roundBottomLeft=*/true, /*roundBottomRight=*/true, Color::Black);
  renderer.drawIcon(CoverIcon, coverRect.x + (coverRect.width - kPlaceholderIconSize) / 2,
                    coverRect.y + coverRect.height / 3 + 14, kPlaceholderIconSize, kPlaceholderIconSize);
  renderer.drawRoundedRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, 1, kCoverCornerRadius, true);
}

void drawBookCover(const GfxRenderer& renderer, const Rect& coverRect, const RecentBook& book,
                   const int coverThumbHeight) {
  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverThumbHeight);
    FsFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const Rect drawn = fittedBitmapRect(bitmap, coverRect);
        renderer.drawBitmap(bitmap, drawn.x, drawn.y, drawn.width, drawn.height);
        renderer.maskRoundedRectOutsideCorners(drawn.x, drawn.y, drawn.width, drawn.height, kCoverCornerRadius,
                                               Color::White);
        renderer.drawRoundedRect(drawn.x, drawn.y, drawn.width, drawn.height, 1, kCoverCornerRadius, true);
        file.close();
        return;
      }
      file.close();
    }
  }
  drawPlaceholderCover(renderer, coverRect);
}

// ---- stats column ----

void drawRightAlignedText(const GfxRenderer& renderer, const int fontId, const int rightX, const int y,
                          const char* text, const bool bold) {
  const EpdFontFamily::Style style = bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rightX - width, y, text, true, style);
}

int statsBlockHeight(const GfxRenderer& renderer) {
  return renderer.getLineHeight(kStatsFontId) + kStatsValueLabelGap + renderer.getLineHeight(kStatsLabelFontId);
}

// Spreads rowCount blocks evenly down the cover's height, distributing the
// leftover pixels one at a time so the last row still lands on the bottom edge.
int statsBlockTop(const Rect& coverRect, const int index, const int blockH, const int rowCount) {
  const int remainingH = std::max(0, coverRect.height - blockH * rowCount);
  const int gapCount = rowCount - 1;
  const int gap = gapCount > 0 ? remainingH / gapCount : 0;
  const int remainder = gapCount > 0 ? remainingH % gapCount : 0;
  return coverRect.y + index * (blockH + gap) + std::min(index, remainder);
}

void drawStatsRow(const GfxRenderer& renderer, const int rightX, const int y, const char* value, const char* label) {
  drawRightAlignedText(renderer, kStatsFontId, rightX, y, value, true);
  drawRightAlignedText(renderer, kStatsLabelFontId, rightX,
                       y + renderer.getLineHeight(kStatsFontId) + kStatsValueLabelGap, label, false);
}

void drawStatsColumn(const GfxRenderer& renderer, const Layout& layout, const ReadingBookStats* book,
                     const int progressPercent) {
  // The date rows need a real calendar, which on both boards means a synced
  // clock -- the X4 has no RTC at all and the X3's only drives the status bar.
  const uint32_t referenceEpoch = TimeUtils::getCurrentValidTimestamp();
  const bool hasClock = TimeUtils::isClockValid(referenceEpoch);
  const int rowCount = hasClock ? 7 : 6;
  const int blockH = statsBlockHeight(renderer);
  const int rightX = layout.statsRightX;

  char value[40];
  char label[48];
  uint64_t estimatedSeconds = 0;
  const bool hasEstimate = estimatedTimeLeft(book, progressPercent, estimatedSeconds);
  const bool isCompleted = book != nullptr && book->completed;
  const uint32_t spanDays = hasClock ? daysReading(book, referenceEpoch) : 0;

  int row = 0;
  formatDuration(bookReadingSeconds(book), value, sizeof(value));
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, row, blockH, rowCount), value, tr(STR_STATS_TIME_LBL));

  if (hasEstimate && !isCompleted) {
    formatCompactDuration(estimatedSeconds, value, sizeof(value));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value, tr(STR_TIME_LEFT_SHORT));

  if (progressPercent >= 0) {
    snprintf(value, sizeof(value), "%d%%", progressPercent);
  } else {
    snprintf(value, sizeof(value), "-");
  }
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value,
               tr(STR_STATS_PROGRESS_LBL));

  if (hasClock) {
    if (spanDays > 0) {
      formatDuration(bookReadingSeconds(book) / spanDays, value, sizeof(value));
    } else {
      snprintf(value, sizeof(value), "-");
    }
    drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value,
                 tr(STR_STATS_DAILY_AVG_LBL));
  }

  snprintf(value, sizeof(value), "%.1f", pagesPerMinute());
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value,
               tr(STR_STATS_PAGES_PER_MIN));

  if (!hasClock) {
    const uint32_t sessions = book != nullptr ? book->sessions : 0;
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(sessions));
    drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value,
                 tr(STR_STATS_SESSIONS_LBL));

    formatDuration(sessions > 0 ? bookReadingSeconds(book) / sessions : 0, value, sizeof(value));
    drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value,
                 tr(STR_STATS_AVG_SESSION_LBL));
    return;
  }

  char dateBuf[24];
  if (spanDays > 0) {
    snprintf(value, sizeof(value), "%lu %s", static_cast<unsigned long>(spanDays),
             spanDays == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS));
  } else {
    snprintf(value, sizeof(value), "-");
  }
  formatShortDate(book != nullptr ? book->firstReadAt : 0, dateBuf, sizeof(dateBuf));
  snprintf(label, sizeof(label), "%s %s", tr(STR_STATS_STARTED), dateBuf);
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), value, label);

  uint32_t finishEpoch = 0;
  if (isCompleted) {
    finishEpoch = book->completedAt;
  } else if (hasEstimate) {
    finishEpoch = estimatedFinishEpoch(book, referenceEpoch, estimatedSeconds);
    if (finishEpoch == 0) {
      finishEpoch = referenceEpoch + static_cast<uint32_t>(std::min<uint64_t>(estimatedSeconds, UINT32_MAX));
    }
  }
  formatShortDate(finishEpoch, dateBuf, sizeof(dateBuf));
  drawStatsRow(renderer, rightX, statsBlockTop(layout.cover, ++row, blockH, rowCount), dateBuf,
               isCompleted ? tr(STR_STATS_FINISHED_DATE) : tr(STR_STATS_EST_FINISH_DATE));
}

// ---- title block ----

void drawBookText(const GfxRenderer& renderer, const Layout& layout, const RecentBook& book,
                  const ReadingBookStats* stats) {
  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int lineH = renderer.getLineHeight(kTitleFontId);
  int y = layout.cover.y + layout.cover.height + kTitleTopGap;

  if (layout.titleLines > 0) {
    for (const auto& line :
         renderer.wrappedText(kTitleFontId, title, layout.textWidth, layout.titleLines, EpdFontFamily::BOLD)) {
      renderer.drawText(kTitleFontId, layout.textLeftX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineH;
    }
  }

  // The chapter is the more useful line when we have it; the author is the
  // fallback for a book that has never been opened on this device.
  const std::string& chapter = stats != nullptr ? stats->chapterTitle : book.author;
  const std::string& subtitle = chapter.empty() ? book.author : chapter;
  if (subtitle.empty() || layout.chapterLines <= 0) {
    return;
  }
  y += kTitleChapterGap;
  for (const auto& line : renderer.wrappedText(kTitleFontId, subtitle.c_str(), layout.textWidth, layout.chapterLines)) {
    renderer.drawText(kTitleFontId, layout.textLeftX, y, line.c_str(), true);
    y += lineH;
  }
}

// ---- footer badges ----

void drawIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int iconX, const int centerY,
                   const char* label, const int maxTextW) {
  const std::string visible = renderer.truncatedText(kFooterFontId, label, maxTextW);
  renderer.drawIcon(icon, iconX, centerY - kFooterIconSize / 2, kFooterIconSize, kFooterIconSize);
  renderer.drawText(kFooterFontId, iconX + kFooterIconSize + kFooterIconTextGap,
                    centerY - renderer.getLineHeight(kFooterFontId) / 2, visible.c_str(), true);
}

void drawRightAlignedIconLabel(const GfxRenderer& renderer, const uint8_t* icon, const int rightX, const int centerY,
                               const char* label, const int maxTextW) {
  const std::string visible = renderer.truncatedText(kFooterFontId, label, maxTextW);
  const int textX = rightX - renderer.getTextWidth(kFooterFontId, visible.c_str());
  renderer.drawIcon(icon, textX - kFooterIconTextGap - kFooterIconSize, centerY - kFooterIconSize / 2, kFooterIconSize,
                    kFooterIconSize);
  renderer.drawText(kFooterFontId, textX, centerY - renderer.getLineHeight(kFooterFontId) / 2, visible.c_str(), true);
}

void drawAnchoredFooterStat(const GfxRenderer& renderer, const int labelX, const int centerY, const char* value,
                            const char* label) {
  const int valueLineH = renderer.getLineHeight(kStatsFontId);
  const int labelLineH = renderer.getLineHeight(kFooterFontId);
  const int valueW = renderer.getTextWidth(kStatsFontId, value, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(kFooterFontId, label);
  const int topY = centerY - (valueLineH + kStatsValueLabelGap + labelLineH) / 2;
  renderer.drawText(kStatsFontId, labelX + (labelW - valueW) / 2, topY, value, true, EpdFontFamily::BOLD);
  renderer.drawText(kFooterFontId, labelX, topY + valueLineH + kStatsValueLabelGap, label, true);
}

void drawFooterStats(const GfxRenderer& renderer, const Layout& layout) {
  const int halfW = std::max(1, (layout.footerRightX - layout.footerLeftX) / 2);
  const int maxTextW = std::max(1, halfW - kFooterIconSize - kFooterIconTextGap);

  // Streak and reader type are both calendar facts; with no clock they would
  // read as a permanent zero, so swap in the two lifetime totals instead.
  if (!TimeUtils::isClockValid()) {
    char totalTime[40];
    char booksRead[16];
    formatDuration(READING_STATS.getTotalReadingMs() / 1000u, totalTime, sizeof(totalTime));
    snprintf(booksRead, sizeof(booksRead), "%lu", static_cast<unsigned long>(READING_STATS.getBooksFinishedCount()));

    drawAnchoredFooterStat(renderer, layout.footerLeftX, layout.footerCenterY, totalTime,
                           tr(STR_STATS_TOTAL_READING_TIME_LBL_SHORT));
    const char* completedLabel = tr(STR_STATS_COMPLETED_LBL);
    drawAnchoredFooterStat(renderer, layout.footerRightX - renderer.getTextWidth(kFooterFontId, completedLabel),
                           layout.footerCenterY, booksRead, completedLabel);
    return;
  }

  char streakBuf[48];
  const uint32_t streak = READING_STATS.getCurrentStreakDays();
  if (streak == 0) {
    snprintf(streakBuf, sizeof(streakBuf), "%s", tr(STR_STATS_NO_STREAK));
  } else {
    snprintf(streakBuf, sizeof(streakBuf), tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(streak));
  }
  drawIconLabel(renderer, Streak24Icon, layout.footerLeftX, layout.footerCenterY, streakBuf, maxTextW);
  drawRightAlignedIconLabel(renderer, readerTypeIcon(), layout.footerRightX, layout.footerCenterY, readerTypeLabel(),
                            maxTextW);
}

}  // namespace

void DashboardTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const ThemeMetrics& metrics = themeMetrics();
  const Layout layout = computeLayout(renderer, rect, metrics);

  if (recentBooks.empty()) {
    renderer.drawRoundedRect(layout.cover.x, layout.cover.y, layout.cover.width, layout.cover.height, 1,
                             kCoverCornerRadius, true);
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  const RecentBook& book = recentBooks[0];

  // The snapshot covers the whole strip, so a restore that failed leaves it
  // blank: redraw from SD in that case, exactly as the carousel does.
  if (!coverRendered || !bufferRestored) {
    // drawBitmap only sets black pixels, so clear first or the previous book's
    // artwork shows through the new one.
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    drawBookCover(renderer, layout.cover, book, metrics.homeCoverHeight);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  // Selection is the whole card here -- there is one book and no menu row in
  // the strip -- so a second outline is all the affordance needed.
  if (selectorIndex == 0) {
    renderer.drawRoundedRect(layout.cover.x - 3, layout.cover.y - 3, layout.cover.width + 6, layout.cover.height + 6, 2,
                             kCoverCornerRadius + 2, true);
  }

  const ReadingBookStats* stats = READING_STATS.findMatchingBookForPath(book.path, book.title, book.author);
  drawStatsColumn(renderer, layout, stats, book.progressPercent);
  drawBookText(renderer, layout, book, stats);
  drawFooterStats(renderer, layout);
}
