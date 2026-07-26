#include "SleepStatsCard.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "ReadingStatsStore.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

namespace SleepStatsCard {

namespace {
// Day of week for a Gregorian date via Sakamoto's algorithm. 0 = Sunday .. 6 = Saturday.
int dayOfWeek(int y, unsigned m, unsigned d) {
  static constexpr int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + static_cast<int>(d)) % 7;
}
}  // namespace

uint8_t buildLines(StatLine lines[MAX_STAT_LINES], bool previewMode) {
  const uint8_t slots[3] = {SETTINGS.sleepStatSlot1, SETTINGS.sleepStatSlot2, SETTINGS.sleepStatSlot3};
  constexpr size_t N = STAT_LINE_LEN;
  uint8_t count = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (count >= MAX_STAT_LINES) break;  // no room for more lines
    lines[count].weekDots = false;
    char* buf = lines[count].text;
    switch (slots[i]) {
      case CrossPointSettings::SLEEP_STAT_TODAY:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_TODAY),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTodayReadingMs()).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_GOAL: {
        const uint64_t todayMs = READING_STATS.getTodayReadingMs();
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        if (todayMs >= goalMs) {
          snprintf(buf, N, "%s", tr(STR_STAT_GOAL_MET));
        } else {
          snprintf(buf, N, "%s: %s / %s", tr(STR_STAT_GOAL), ReadingStatsAnalytics::formatDurationHm(todayMs).c_str(),
                   ReadingStatsAnalytics::formatDurationHm(goalMs).c_str());
        }
        break;
      }
      case CrossPointSettings::SLEEP_STAT_WEEK:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_WEEK),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(7)).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_MONTH:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_MONTH),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(30)).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_STREAK:
        snprintf(buf, N, "%s: %u %s", tr(STR_STAT_STREAK), static_cast<unsigned>(READING_STATS.getCurrentStreakDays()),
                 tr(STR_STAT_DAYS));
        break;
      case CrossPointSettings::SLEEP_STAT_TOTAL:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_TOTAL),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTotalReadingMs()).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_FINISHED:
        snprintf(buf, N, "%s: %u", tr(STR_STAT_FINISHED), static_cast<unsigned>(READING_STATS.getBooksFinishedCount()));
        break;
      case CrossPointSettings::SLEEP_STAT_RETURN: {
        const bool haveContact = SETTINGS.returnContact[0] != '\0';
        if (!haveContact && !previewMode) {
          continue;  // no contact entered -> nothing to show
        }
        // Two lines: the "please return to" label, then the contact on its own
        // line so a full email address has room. (The post-switch count++ below
        // accounts for the label line; this consumes the extra contact line.)
        // In preview with no contact set, show a bracketed placeholder so the
        // user sees where their contact string will appear.
        snprintf(lines[count].text, N, "%s", tr(STR_STAT_RETURN_MSG));
        if (count + 1 < MAX_STAT_LINES) {
          lines[count + 1].weekDots = false;
          if (haveContact) {
            snprintf(lines[count + 1].text, N, "%s", SETTINGS.returnContact);
          } else {
            snprintf(lines[count + 1].text, N, "[%s]", tr(STR_RETURN_CONTACT));
          }
          count++;
        }
        break;
      }
      case CrossPointSettings::SLEEP_STAT_BOOK_PROGRESS: {
        const ReadingBookStats* book =
            APP_STATE.openEpubPath.empty() ? nullptr : READING_STATS.findMatchingBookForPath(APP_STATE.openEpubPath);
        if (book == nullptr) {
          if (!previewMode) continue;  // no book open / not tracked yet -> nothing to show
          snprintf(buf, N, "%s: %u%%", tr(STR_STAT_BOOK_PROGRESS), 42u);  // sample
          break;
        }
        snprintf(buf, N, "%s: %u%%", tr(STR_STAT_BOOK_PROGRESS), static_cast<unsigned>(book->lastProgressPercent));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_BOOK_TIME_LEFT: {
        // Prefer the reader's own pages-left × pace figure, stashed at reader
        // exit — the same estimate the status bar / reader menu show, and it
        // doesn't hide after short sessions the way percent-based pace does.
        // Fallback: pace of the just-finished session. Preview: a sample value.
        uint64_t timeLeftMs = 0;
        bool haveEstimate = false;
        if (!APP_STATE.openEpubPath.empty()) {
          if (APP_STATE.readerTimeLeftSeconds > 0 && APP_STATE.readerTimeLeftBookPath == APP_STATE.openEpubPath) {
            timeLeftMs = static_cast<uint64_t>(APP_STATE.readerTimeLeftSeconds) * 1000ULL;
            haveEstimate = true;
          } else {
            // "read (end-start)% of the book in sessionMs, so (100-end)% remaining
            // takes remaining/pace". Recent-reading only, so it survives a stats
            // reset, unlike extrapolating lifetime time vs. progress.
            const ReadingSessionSnapshot& snap = READING_STATS.getLastSessionSnapshot();
            if (snap.valid && snap.path == APP_STATE.openEpubPath && snap.sessionMs != 0 &&
                snap.endProgressPercent > snap.startProgressPercent && snap.endProgressPercent < 100) {
              const uint32_t gainedPct = snap.endProgressPercent - snap.startProgressPercent;
              const uint32_t remainingPct = 100 - snap.endProgressPercent;
              timeLeftMs = static_cast<uint64_t>(snap.sessionMs) * remainingPct / gainedPct;
              haveEstimate = true;
            }
          }
        }
        if (!haveEstimate) {
          if (!previewMode) continue;                   // no recent forward progress to estimate a pace from
          timeLeftMs = 2ULL * 60ULL * 60ULL * 1000ULL;  // sample: 2h
        }
        snprintf(buf, N, "%s %s", ReadingStatsAnalytics::formatDurationHm(timeLeftMs).c_str(),
                 tr(STR_STAT_BOOK_TIME_LEFT_MSG));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_GOAL_LEFT: {
        const uint64_t todayMs = READING_STATS.getTodayReadingMs();
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        if (todayMs >= goalMs) {
          snprintf(buf, N, "%s", tr(STR_STAT_GOAL_MET));
        } else {
          snprintf(buf, N, "%s %s", ReadingStatsAnalytics::formatDurationHm(goalMs - todayMs).c_str(),
                   tr(STR_STAT_GOAL_LEFT_MSG));
        }
        break;
      }
      case CrossPointSettings::SLEEP_STAT_DAILY_AVG:
        snprintf(buf, N, "%s: %s", tr(STR_STAT_DAILY_AVG),
                 ReadingStatsAnalytics::formatDurationHm(READING_STATS.getRecentReadingMs(7) / 7).c_str());
        break;
      case CrossPointSettings::SLEEP_STAT_DAYS_MONTH: {
        // Count reading days whose (year, month) matches the reference date —
        // mirrors buildMonthSummary() in ReadingStatsActivity.
        const uint32_t refOrdinal = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
        int refYear = 0;
        unsigned refMonth = 0, refDay = 0;
        if (!TimeUtils::getDateFromDayOrdinal(refOrdinal, refYear, refMonth, refDay)) {
          if (!previewMode) continue;                                // clock not set -> can't scope to a month
          snprintf(buf, N, "%s: %u", tr(STR_STAT_DAYS_MONTH), 12u);  // sample
          break;
        }
        uint32_t daysRead = 0;
        for (const auto& d : READING_STATS.getReadingDays()) {
          if (d.readingMs == 0) continue;
          int y = 0;
          unsigned m = 0, dd = 0;
          if (TimeUtils::getDateFromDayOrdinal(d.dayOrdinal, y, m, dd) && y == refYear && m == refMonth) {
            daysRead++;
          }
        }
        snprintf(buf, N, "%s: %u", tr(STR_STAT_DAYS_MONTH), static_cast<unsigned>(daysRead));
        break;
      }
      case CrossPointSettings::SLEEP_STAT_WEEK_STREAK: {
        // Mon-Sun of the current week. Bit d (0 = Monday .. 6 = Sunday) is set when
        // that weekday met the daily goal. Future days this week simply stay unset.
        const uint32_t todayOrd = TimeUtils::getLocalDayOrdinal(READING_STATS.getDisplayTimestamp());
        int ty = 0;
        unsigned tm = 0, tdd = 0;
        if (!TimeUtils::getDateFromDayOrdinal(todayOrd, ty, tm, tdd)) {
          if (!previewMode) continue;  // clock not set -> can't anchor the week
          lines[count].weekDots = true;
          lines[count].weekMask = 0b0011111;  // sample: Mon-Fri met
          lines[count].weekCount = 7;
          lines[count].text[0] = '\0';
          break;
        }
        const int dow = dayOfWeek(ty, tm, tdd);  // 0 = Sunday
        const uint32_t mondayOrd = todayOrd - static_cast<uint32_t>((dow + 6) % 7);
        const uint64_t goalMs = SETTINGS.getDailyGoalMs();
        uint8_t mask = 0;
        for (uint8_t d = 0; d < 7; d++) {  // Monday..Sunday
          const uint32_t ord = mondayOrd + d;
          uint64_t ms = 0;
          for (const auto& rd : READING_STATS.getReadingDays()) {
            if (rd.dayOrdinal == ord) {
              ms = rd.readingMs;
              break;
            }
          }
          if (ms >= goalMs) mask = static_cast<uint8_t>(mask | (1u << d));
        }
        lines[count].weekDots = true;
        lines[count].weekMask = mask;
        lines[count].weekCount = 7;
        lines[count].text[0] = '\0';
        break;
      }
      default:  // SLEEP_STAT_NONE or out-of-range: skip this slot
        continue;
    }
    count++;
  }
  return count;
}

void draw(GfxRenderer& renderer, bool clearRegionOnly, int anchorBottomY, bool previewMode) {
  StatLine lines[MAX_STAT_LINES];
  const uint8_t count = buildLines(lines, previewMode);
  if (count == 0) return;  // no stats selected -> leave the screen untouched

  // A single rounded card (home-menu style) holding all the stat lines.
  constexpr int STATS_FONT_ID = UI_12_FONT_ID;
  constexpr int LETTER_FONT_ID = SMALL_FONT_ID;  // weekday letters inside the cells
  constexpr int RADIUS = 8;
  constexpr int BORDER = 2;
  constexpr int PAD_X = 24;
  constexpr int PAD_Y = 14;
  constexpr int LINE_GAP = 6;
  constexpr int CELL_GAP = 6;  // gap between weekly-streak cells
  static constexpr char WEEK_LETTERS[7] = {'M', 'T', 'W', 'T', 'F', 'S', 'S'};

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(STATS_FONT_ID);
  const int cell = lineH;  // weekly-streak cell = one line tall
  auto weekRowWidth = [&](uint8_t cells) { return cells * cell + (cells - 1) * CELL_GAP; };

  // Box width sized to the widest line/row (clamped to the screen).
  int maxLineW = 0;
  for (uint8_t i = 0; i < count; i++) {
    const int w =
        lines[i].weekDots ? weekRowWidth(lines[i].weekCount) : renderer.getTextWidth(STATS_FONT_ID, lines[i].text);
    if (w > maxLineW) maxLineW = w;
  }
  int boxW = maxLineW + 2 * PAD_X;
  const int maxBoxW = pageWidth - 8;
  if (boxW > maxBoxW) boxW = maxBoxW;
  const int boxH = count * lineH + (count - 1) * LINE_GAP + 2 * PAD_Y;
  const int boxX = (pageWidth - boxW) / 2;
  // Bottom edge: caller-supplied anchor, or the lower ~1/8 margin by default (not
  // flush to the edge). The box grows upward from that bottom edge.
  const int bottomAnchor = (anchorBottomY >= 0) ? anchorBottomY : (pageHeight - pageHeight / 8);
  const int boxY = bottomAnchor - boxH;

  // Greyscale compositing: the box is painted into the BW base image, and each
  // greyscale pass just clears the box rectangle so displayGrayBuffer leaves that
  // region untouched (keeping the BW box) while the rest renders in greyscale.
  if (clearRegionOnly) {
    renderer.fillRect(boxX, boxY, boxW, boxH, /*state=*/true);  // clear both planes -> "no change"
    return;
  }

  // White fill + black rounded border + black centred lines. Runs in BW render
  // mode; on the dark default screen the caller's invertScreen() then flips the
  // box to a white outline + white text/cells on black. Centred content lines up
  // with the screen-centred box.
  renderer.fillRoundedRect(boxX, boxY, boxW, boxH, RADIUS, Color::White);
  renderer.drawRoundedRect(boxX, boxY, boxW, boxH, BORDER, RADIUS, /*state=*/true);
  for (uint8_t i = 0; i < count; i++) {
    const int lineTop = boxY + PAD_Y + i * (lineH + LINE_GAP);
    if (lines[i].weekDots) {
      // A labelled cell per weekday: draw the letter, then fill (which hides the
      // letter) when the goal was met, or just outline it when it wasn't.
      const uint8_t cells = lines[i].weekCount;
      const int startX = (pageWidth - weekRowWidth(cells)) / 2;
      const int letterH = renderer.getLineHeight(LETTER_FONT_ID);
      for (uint8_t d = 0; d < cells; d++) {
        const int cx = startX + d * (cell + CELL_GAP);
        const char letter[2] = {WEEK_LETTERS[d], '\0'};
        const int letterW = renderer.getTextWidth(LETTER_FONT_ID, letter);
        renderer.drawText(LETTER_FONT_ID, cx + (cell - letterW) / 2, lineTop + (cell - letterH) / 2, letter,
                          /*black=*/true);
        if (lines[i].weekMask & (1u << d)) {
          renderer.fillRect(cx, lineTop, cell, cell, /*state=*/true);
        } else {
          renderer.drawRect(cx, lineTop, cell, cell, /*state=*/true);
        }
      }
    } else {
      // drawText/drawCenteredText take the text TOP (they add the ascender
      // internally), so pass the line's top edge inside the box.
      renderer.drawCenteredText(STATS_FONT_ID, lineTop, lines[i].text, /*black=*/true);
    }
  }
}

}  // namespace SleepStatsCard
