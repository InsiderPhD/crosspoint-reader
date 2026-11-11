#include "LicoriceTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_random.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/themes/CoverCache.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"

const ThemeMetrics& LicoriceTheme::themeMetrics() const {
#if FREEINK_CAP_TOUCH
  // See BaseTheme::themeMetrics().
  return CrossPointSettings::FULL_TOUCH_UI ? LicoriceMetrics::values : LicoriceMetrics::noActionBarValues;
#else
  return LicoriceMetrics::values;
#endif
}

namespace {

bool contains(const Rect& r, const int x, const int y) {
  return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

}  // namespace

int LicoriceTheme::hitTestRecentBookCover(const Rect& rect, const int slotCount, const int lx, const int ly) const {
  (void)rect;  // the bar was laid out (and recorded) by the last draw
  return slotCount > 0 && contains(bookBar, lx, ly) ? 0 : -1;
}

int LicoriceTheme::hitTestButtonMenu(const Rect& rect, const int buttonCount, const int lx, const int ly) const {
  (void)rect;  // the menu lives in the folder card and the month card, not in rect
  if (statsMenuIndex >= 0 && statsMenuIndex < buttonCount && contains(statsCard, lx, ly)) return statsMenuIndex;
  if (clipMenuIndex >= 0 && clipMenuIndex < buttonCount && contains(clipCard, lx, ly)) return clipMenuIndex;
  for (int k = 0; k < kFolderSlots; ++k) {
    if (folderMenuIndex[k] >= 0 && folderMenuIndex[k] < buttonCount && contains(folderTiles[k], lx, ly)) {
      return folderMenuIndex[k];
    }
  }
  return -1;
}

// Positions: the month card on top, the folder's 2x2 grid under it with the
// clipping card to its right, the book bar at the bottom. A move off an edge stays put (returns selectorIndex);
// -1 falls back to the linear order, used only before the first draw has
// recorded the menu or for Left/Right on the book.
int LicoriceTheme::homeNavigate(const int selectorIndex, const int coverCount, const int totalCount,
                                const NavDirection dir) const {
  (void)totalCount;
  const auto selectorOf = [&](const int menuIndex) { return menuIndex >= 0 ? coverCount + menuIndex : -1; };
  const auto folder = [&](const int k) { return selectorOf(folderMenuIndex[k]); };
  const auto orStay = [&](const int target) { return target >= 0 ? target : selectorIndex; };
  const int book = coverCount > 0 ? 0 : -1;
  const int stats = selectorOf(statsMenuIndex);
  const int clip = selectorOf(clipMenuIndex);

  if (selectorIndex < coverCount) {
    if (dir == NavDirection::Up) return orStay(folder(2) >= 0 ? folder(2) : folder(0));
    if (dir == NavDirection::Down) return selectorIndex;
    return -1;
  }
  if (stats >= 0 && selectorIndex == stats) {
    return dir == NavDirection::Down ? orStay(folder(0)) : selectorIndex;
  }
  if (clip >= 0 && selectorIndex == clip) {
    switch (dir) {
      case NavDirection::Up:
        return orStay(stats);
      case NavDirection::Down:
        return orStay(book);
      case NavDirection::Left:
        return orStay(folder(1) >= 0 ? folder(1) : folder(0));
      case NavDirection::Right:
        return selectorIndex;
    }
  }
  for (int k = 0; k < kFolderSlots; ++k) {
    if (folder(k) != selectorIndex) continue;
    const int row = k / 2;
    const int col = k % 2;
    switch (dir) {
      case NavDirection::Up:
        return row == 0 ? orStay(stats) : orStay(folder(col));
      case NavDirection::Down:
        return row == 1 ? orStay(book) : orStay(folder(2 + col));
      case NavDirection::Left:
        return col == 1 ? orStay(folder(row * 2)) : selectorIndex;
      case NavDirection::Right:
        return col == 0 ? orStay(folder(row * 2 + 1)) : orStay(clip);
    }
  }
  return -1;
}

namespace {

constexpr int kDateFontId = UI_12_FONT_ID;
constexpr int kSuffixFontId = UI_12_FONT_ID;
constexpr int kCardTitleFontId = UI_12_FONT_ID;
constexpr int kCardBodyFontId = UI_12_FONT_ID;
constexpr int kCardSmallFontId = SMALL_FONT_ID;
constexpr int kBookTitleFontId = UI_12_FONT_ID;
constexpr int kBookMetaFontId = UI_10_FONT_ID;
constexpr int kQuoteFontId = UI_10_FONT_ID;

constexpr int kSideInset = 20;
constexpr int kTopGap = 6;
constexpr int kBottomGap = 12;
constexpr int kSectionGap = 12;
constexpr int kColumnGap = 24;
constexpr int kDateGap = 10;
constexpr int kClockDigitH = 76;
constexpr int kClockDigitHWide = 56;

constexpr int kCoverRadius = 6;
constexpr int kCardGap = 12;
constexpr int kStatsCardH = 190;  // fits five grid rows beside the ring and streak
constexpr int kMonthShare = 55;   // percent of the stats card given to This month
constexpr int kCardPad = 12;
constexpr int kCardRadius = 10;
constexpr int kCardStroke = 2;
constexpr int kSelectedCardStroke = 4;
constexpr int kMaxGridCell = 28;
constexpr int kMaxRingRadius = 46;
constexpr int kRingPen = 4;
constexpr int kRingGap = 8;
constexpr int kTextGap = 14;

// Folder: four live tiles, icon only.
constexpr int kMenuIconSize = 32;  // the bitmaps' size; drawIcon does not scale

constexpr StrId kWeekdayShort[] = {StrId::STR_WEEKDAY_SHORT_SUN, StrId::STR_WEEKDAY_SHORT_MON,
                                   StrId::STR_WEEKDAY_SHORT_TUE, StrId::STR_WEEKDAY_SHORT_WED,
                                   StrId::STR_WEEKDAY_SHORT_THU, StrId::STR_WEEKDAY_SHORT_FRI,
                                   StrId::STR_WEEKDAY_SHORT_SAT};
constexpr StrId kMonthShort[] = {StrId::STR_MONTH_SHORT_JAN, StrId::STR_MONTH_SHORT_FEB, StrId::STR_MONTH_SHORT_MAR,
                                 StrId::STR_MONTH_SHORT_APR, StrId::STR_MONTH_SHORT_MAY, StrId::STR_MONTH_SHORT_JUN,
                                 StrId::STR_MONTH_SHORT_JUL, StrId::STR_MONTH_SHORT_AUG, StrId::STR_MONTH_SHORT_SEP,
                                 StrId::STR_MONTH_SHORT_OCT, StrId::STR_MONTH_SHORT_NOV, StrId::STR_MONTH_SHORT_DEC};

// tr() pastes its argument onto StrId::, so table lookups go through here.
const char* trId(const StrId id) { return I18n::getInstance().get(id); }

int bookRowHeight(const GfxRenderer& renderer);  // with the book row, below

// ---- layout ----

struct Layout {
  Rect clock;  // digits, then the date line under them
  int digitH = 0;
  Rect stats;  // one long card: This month on the left, Today on the right
  Rect month;  // the two halves of stats, unframed
  Rect today;
  Rect folder;
  Rect clip;
  Rect bar;
};

// Splits the long stats card into its This month and Today halves.
void splitStats(Layout& l) {
  const int monthW = (l.stats.width * kMonthShare) / 100;
  l.month = Rect{l.stats.x, l.stats.y, monthW, l.stats.height};
  l.today = Rect{l.stats.x + monthW, l.stats.y, l.stats.width - monthW, l.stats.height};
}

// Two cards side by side across [x, x + width).
void cardRow(Rect& left, Rect& right, const int x, const int width, const int y, const int h) {
  const int halfW = (width - kCardGap) / 2;
  left = Rect{x, y, halfW, h};
  right = Rect{x + halfW + kCardGap, y, width - halfW - kCardGap, h};
}

// Portrait stacks clock / month+today / folder+clipping / book bar, the card
// rows square-ish and any spare height shared between the gaps. A wide frame
// is barely half as tall, so it splits into two columns instead.
Layout computeLayout(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x0 = kSideInset + left;
  const int width = std::max(1, renderer.getScreenWidth() - kSideInset - right - x0);
  const int y0 = rect.y + kTopGap;
  // Home hides the hint bar (homeHideButtonHints), so the layout takes its
  // strip, stopping short of the panel's bottom bezel margin.
  const int y1 =
      renderer.getScreenHeight() - bottom - (metrics.homeHideButtonHints ? 0 : metrics.buttonHintsHeight) - kBottomGap;
  const int dateH = renderer.getLineHeight(kDateFontId);
  const int barH = bookRowHeight(renderer);

  Layout l;
  if (renderer.getScreenWidth() <= renderer.getScreenHeight()) {
    l.digitH = kClockDigitH;
    l.clock = Rect{x0, y0, width, l.digitH + kDateGap + dateH};
    l.bar = Rect{x0, y1 - barH, width, barH};
    const int cardsTop = l.clock.y + l.clock.height + kSectionGap;
    const int cardsH = l.bar.y - kSectionGap - cardsTop;
    // The stats card takes what its grid and ring need; the folder row gets the
    // rest, up to square tiles, and any spare height is shared between the gaps.
    const int statsH = std::max(1, std::min(kStatsCardH, (cardsH - kCardGap) / 2));
    const int rowH = std::max(1, std::min((width - kCardGap) / 2, cardsH - kCardGap - statsH));
    const int spare = std::max(0, cardsH - kCardGap - statsH - rowH) / 3;
    l.stats = Rect{x0, cardsTop + spare, width, statsH};
    cardRow(l.folder, l.clip, x0, width, cardsTop + 2 * spare + statsH + kCardGap, rowH);
  } else {
    const int colW = (width - kColumnGap) / 2;
    const int rightX = x0 + colW + kColumnGap;
    const int rightW = width - colW - kColumnGap;
    l.digitH = kClockDigitHWide;
    l.clock = Rect{x0, y0, colW, l.digitH + kDateGap + dateH};
    const int monthTop = l.clock.y + l.clock.height + kSectionGap;
    l.stats = Rect{x0, monthTop, colW, std::max(1, y1 - monthTop)};
    l.bar = Rect{rightX, y1 - barH, rightW, barH};
    cardRow(l.folder, l.clip, rightX, rightW, y0, std::max(1, l.bar.y - kSectionGap - y0));
  }
  splitStats(l);
  return l;
}

// ---- wall clock ----
//
// Digits are polylines and elliptical arcs on a 60 x 100 unit box, traced with
// a round pen. Angles are degrees, clockwise from 3 o'clock (screen y is down).

constexpr double kPi = 3.14159265358979323846;

constexpr double taylorSin(const double x) {
  double term = x;
  double sum = x;
  for (int n = 1; n < 12; ++n) {
    term *= -x * x / ((2.0 * n) * (2.0 * n + 1.0));
    sum += term;
  }
  return sum;
}

// sin(0..90 degrees) in Q14, built at compile time so the arcs need no float.
constexpr std::array<int16_t, 91> kSinQ14 = [] {
  std::array<int16_t, 91> t{};
  for (int d = 0; d <= 90; ++d) t[d] = static_cast<int16_t>(taylorSin(d * kPi / 180.0) * 16384.0 + 0.5);
  return t;
}();

int sinQ14(int deg) {
  deg %= 360;
  if (deg < 0) deg += 360;
  if (deg <= 90) return kSinQ14[deg];
  if (deg <= 180) return kSinQ14[180 - deg];
  if (deg <= 270) return -kSinQ14[deg - 180];
  return -kSinQ14[360 - deg];
}
int cosQ14(const int deg) { return sinQ14(deg + 90); }

enum class StrokeKind : uint8_t { Line, Arc, End };

// Line: (a, b) -> (c, d). Arc: centre (a, b), radii (c, d), from e to f degrees.
struct Stroke {
  StrokeKind kind;
  int16_t a, b, c, d, e, f;
};

constexpr Stroke L(int a, int b, int c, int d) {
  return {StrokeKind::Line,
          static_cast<int16_t>(a),
          static_cast<int16_t>(b),
          static_cast<int16_t>(c),
          static_cast<int16_t>(d),
          0,
          0};
}
constexpr Stroke A(int cx, int cy, int rx, int ry, int from, int to) {
  return {StrokeKind::Arc,          static_cast<int16_t>(cx),   static_cast<int16_t>(cy), static_cast<int16_t>(rx),
          static_cast<int16_t>(ry), static_cast<int16_t>(from), static_cast<int16_t>(to)};
}
constexpr Stroke kEnd = {StrokeKind::End, 0, 0, 0, 0, 0, 0};

// Glyphs 0-9 then '-', each run terminated by kEnd.
constexpr Stroke kGlyphStrokes[] = {
    A(30, 50, 27, 47, 0, 360),
    kEnd,  // 0
    L(30, 3, 30, 97),
    L(30, 3, 15, 18),
    L(17, 97, 43, 97),
    kEnd,  // 1
    A(30, 28, 26, 25, 165, 400),
    L(50, 44, 3, 97),
    L(3, 97, 58, 97),
    kEnd,  // 2
    A(30, 27, 25, 24, 195, 450),
    A(30, 74, 27, 24, 270, 520),
    kEnd,  // 3
    L(44, 97, 44, 3),
    L(44, 3, 3, 68),
    L(3, 68, 58, 68),
    kEnd,  // 4
    L(54, 3, 12, 3),
    L(12, 3, 8, 47),
    A(30, 68, 27, 29, 220, 520),
    kEnd,  // 5
    A(30, 70, 27, 27, 0, 360),
    A(30, 70, 27, 67, 180, 285),
    kEnd,  // 6
    L(3, 3, 57, 3),
    L(57, 3, 22, 97),
    kEnd,  // 7
    A(30, 26, 22, 23, 0, 360),
    A(30, 74, 27, 23, 0, 360),
    kEnd,  // 8
    A(30, 30, 27, 27, 0, 360),
    A(30, 30, 27, 67, 0, 105),
    kEnd,  // 9
    L(12, 50, 48, 50),
    kEnd,  // -
};
constexpr int kGlyphCount = 11;

constexpr std::array<uint8_t, kGlyphCount> kGlyphStart = [] {
  std::array<uint8_t, kGlyphCount> starts{};
  int glyph = 0;
  starts[0] = 0;
  for (int i = 0; i < static_cast<int>(sizeof(kGlyphStrokes) / sizeof(kGlyphStrokes[0])) && glyph + 1 < kGlyphCount;
       ++i) {
    if (kGlyphStrokes[i].kind == StrokeKind::End) starts[++glyph] = static_cast<uint8_t>(i + 1);
  }
  return starts;
}();

constexpr int kGlyphUnitsW = 60;
constexpr int kGlyphGapUnits = 20;
constexpr int kColonUnitsW = 28;
constexpr int kArcStepDeg = 6;
constexpr int kMaxPenRadius = 8;

// A round nib: each dot is 2r+1 one-row fillRects, so a stroke has round caps
// and joins for free and stays the same weight at any angle.
class Pen {
 public:
  Pen(const GfxRenderer& renderer, const int radius) : renderer(renderer), r(std::clamp(radius, 1, kMaxPenRadius)) {
    for (int dy = -r; dy <= r; ++dy) {
      int w = 0;
      while ((w + 1) * (w + 1) + dy * dy <= r * r) ++w;
      half[dy + r] = static_cast<int8_t>(w);
    }
  }

  void dot(const int x, const int y) const {
    for (int dy = -r; dy <= r; ++dy) {
      const int w = half[dy + r];
      renderer.fillRect(x - w, y + dy, 2 * w + 1, 1, true);
    }
  }

  // Dots every r/2 px: the discs overlap enough to leave a solid stroke at a
  // quarter of the fills a per-pixel walk would cost.
  void line(const int x0, const int y0, const int x1, const int y1) const {
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int len = std::max(std::abs(dx), std::abs(dy));
    const int step = std::max(1, r / 2);
    for (int i = 0; i < len; i += step) dot(x0 + dx * i / len, y0 + dy * i / len);
    dot(x1, y1);
  }

  int radius() const { return r; }

 private:
  const GfxRenderer& renderer;
  int r;
  int8_t half[2 * kMaxPenRadius + 1] = {};
};

// Glyph units -> pixels for a digit box at (ox, oy), h px tall. Q14 inputs
// keep the arc points sub-unit accurate before the one rounding.
int unitToPx(const int origin, const int64_t unitsQ14, const int h) {
  return origin + static_cast<int>((unitsQ14 * h + 50 * 16384) / (100 * 16384));
}

void drawGlyph(const Pen& pen, const int glyph, const int ox, const int oy, const int h) {
  for (int i = kGlyphStart[glyph]; kGlyphStrokes[i].kind != StrokeKind::End; ++i) {
    const Stroke& s = kGlyphStrokes[i];
    if (s.kind == StrokeKind::Line) {
      pen.line(unitToPx(ox, s.a * 16384LL, h), unitToPx(oy, s.b * 16384LL, h), unitToPx(ox, s.c * 16384LL, h),
               unitToPx(oy, s.d * 16384LL, h));
      continue;
    }
    int prevX = 0;
    int prevY = 0;
    for (int deg = s.e;; deg = std::min(deg + kArcStepDeg, static_cast<int>(s.f))) {
      const int x = unitToPx(ox, s.a * 16384LL + static_cast<int64_t>(s.c) * cosQ14(deg), h);
      const int y = unitToPx(oy, s.b * 16384LL + static_cast<int64_t>(s.d) * sinQ14(deg), h);
      if (deg == s.e) {
        pen.dot(x, y);
      } else {
        pen.line(prevX, prevY, x, y);
      }
      prevX = x;
      prevY = y;
      if (deg >= s.f) break;
    }
  }
}

int glyphIndex(const char c) { return c == '-' ? 10 : c - '0'; }

int clockTextWidth(const char* text, const int h) {
  int units = 0;
  for (const char* p = text; *p; ++p) {
    units += (*p == ':') ? kColonUnitsW : kGlyphUnitsW + kGlyphGapUnits;
  }
  if (*text && text[strlen(text) - 1] != ':') units -= kGlyphGapUnits;
  return (units * h) / 100;
}

void drawClockText(const GfxRenderer& renderer, const char* text, int x, const int y, const int h) {
  const Pen pen(renderer, h / 15);
  for (const char* p = text; *p; ++p) {
    if (*p == ':') {
      const int cx = x + (kColonUnitsW * h) / 200;
      Pen dotPen(renderer, pen.radius() + 1);
      dotPen.dot(cx, y + (34 * h) / 100);
      dotPen.dot(cx, y + (74 * h) / 100);
      x += (kColonUnitsW * h) / 100;
      continue;
    }
    drawGlyph(pen, glyphIndex(*p), x, y, h);
    x += ((kGlyphUnitsW + kGlyphGapUnits) * h) / 100;
  }
}

bool localNow(tm& out, uint32_t& epochOut) {
  epochOut = TimeUtils::getCurrentValidTimestamp();
  if (!TimeUtils::isClockValid(epochOut)) return false;
  TimeUtils::configureTimezone();
  const time_t t = static_cast<time_t>(epochOut);
  return localtime_r(&t, &out) != nullptr;
}

// Digits h px tall centred across area.width from area.y, the date under them.
void drawClock(const GfxRenderer& renderer, const Rect& area, const int h, const bool hasTime, const tm& now) {
  char digits[8];
  const char* suffix = nullptr;
  if (!hasTime) {
    snprintf(digits, sizeof(digits), "--:--");
  } else if (SETTINGS.clockFormat == 1) {
    const int hour12 = now.tm_hour % 12 == 0 ? 12 : now.tm_hour % 12;
    snprintf(digits, sizeof(digits), "%d:%02d", hour12, now.tm_min);
    suffix = now.tm_hour < 12 ? "AM" : "PM";  // as HalClock::formatTime prints it
  } else {
    snprintf(digits, sizeof(digits), "%02d:%02d", now.tm_hour, now.tm_min);
  }

  constexpr int kSuffixGap = 8;
  const int digitsW = clockTextWidth(digits, h);
  const int suffixW = suffix ? kSuffixGap + renderer.getTextWidth(kSuffixFontId, suffix, EpdFontFamily::BOLD) : 0;
  const int x = area.x + (area.width - digitsW - suffixW) / 2;
  drawClockText(renderer, digits, x, area.y, h);
  if (suffix) {
    renderer.drawText(kSuffixFontId, x + digitsW + kSuffixGap, area.y, suffix, true, EpdFontFamily::BOLD);
  }

  if (!hasTime) return;
  char date[32];
  snprintf(date, sizeof(date), "%s %d %s", trId(kWeekdayShort[now.tm_wday % 7]), now.tm_mday,
           trId(kMonthShort[now.tm_mon % 12]));
  const int dateW = renderer.getTextWidth(kDateFontId, date);
  renderer.drawText(kDateFontId, area.x + (area.width - dateW) / 2, area.y + h + kDateGap, date, true);
}

// ---- cards ----

void drawCardFrame(const GfxRenderer& renderer, const Rect& r) {
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, kCardStroke, kCardRadius, true);
}

void drawSelectedFrame(const GfxRenderer& renderer, const Rect& r) {
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, kSelectedCardStroke, kCardRadius, true);
}

void drawDottedRect(const GfxRenderer& renderer, const Rect& r, const int spacing) {
  for (int x = r.x; x < r.x + r.width; x += spacing) {
    renderer.drawPixel(x, r.y);
    renderer.drawPixel(x, r.y + r.height - 1);
  }
  for (int y = r.y; y < r.y + r.height; y += spacing) {
    renderer.drawPixel(r.x, y);
    renderer.drawPixel(r.x + r.width - 1, y);
  }
}

int daysInMonth(const int year, const unsigned month) {
  const uint32_t first = TimeUtils::getDayOrdinalForDate(year, month, 1);
  const uint32_t next = month == 12 ? TimeUtils::getDayOrdinalForDate(year + 1, 1, 1)
                                    : TimeUtils::getDayOrdinalForDate(year, month + 1, 1);
  return (first != 0 && next > first) ? static_cast<int>(next - first) : 0;
}

// This month as day squares from the 1st (a habit grid, not a wall calendar):
// read days filled, today ringed, days to come dotted. Unframed: the left half
// of the stats card.
void drawMonthCard(const GfxRenderer& renderer, const Rect& r, const bool hasTime, const tm& now) {
  const int x = r.x + kCardPad;
  const int y = r.y + kCardPad;
  const int innerW = r.width - 2 * kCardPad;
  const char* title = tr(STR_THIS_MONTH);
  renderer.drawText(kCardTitleFontId, x, y, title, true);
  if (!hasTime) return;

  const int year = now.tm_year + 1900;
  const unsigned month = static_cast<unsigned>(now.tm_mon + 1);
  const int days = std::min(daysInMonth(year, month), 31);
  if (days <= 0) return;
  const uint32_t firstOrdinal = TimeUtils::getDayOrdinalForDate(year, month, 1);

  // One bit per day of the month; the store keeps a short rolling window of
  // days, so this is a few dozen compares.
  uint32_t readMask = 0;
  if (SETTINGS.trackReadingStats) {
    for (const ReadingDayStats& day : READING_STATS.getReadingDays()) {
      if (day.readingMs > 0 && day.dayOrdinal >= firstOrdinal && day.dayOrdinal < firstOrdinal + days) {
        readMask |= 1u << (day.dayOrdinal - firstOrdinal);
      }
    }
  }

  const int readCount = __builtin_popcount(readMask);
  char count[24];
  snprintf(count, sizeof(count), "%d %s", readCount, readCount == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS));
  const int countW = renderer.getTextWidth(kCardSmallFontId, count);
  const int titleH = renderer.getLineHeight(kCardTitleFontId);
  constexpr int kCountGap = 10;
  if (renderer.getTextWidth(kCardTitleFontId, title) + kCountGap + countW <= innerW) {  // dropped on a narrow card
    renderer.drawText(kCardSmallFontId, x + innerW - countW,
                      y + (titleH - renderer.getLineHeight(kCardSmallFontId)) / 2, count, true);
  }

  constexpr int kCols = 7;
  const int rows = (days + kCols - 1) / kCols;
  const int gridTop = y + titleH + 10;
  const int gridH = r.y + r.height - kCardPad - gridTop;
  const int cell = std::min({kMaxGridCell, innerW / kCols, gridH / rows});
  if (cell < 6) return;
  const int square = cell - std::max(3, cell / 5);
  const int todayIdx = now.tm_mday - 1;

  for (int d = 0; d < days; ++d) {
    const Rect sq{x + (d % kCols) * cell, gridTop + (d / kCols) * cell, square, square};
    if (readMask & (1u << d)) {
      renderer.fillRoundedRect(sq.x, sq.y, sq.width, sq.height, 3, Color::Black);
    } else if (d == todayIdx) {
      renderer.drawRoundedRect(sq.x, sq.y, sq.width, sq.height, 2, 3, true);
    } else if (d < todayIdx) {
      renderer.drawRoundedRect(sq.x, sq.y, sq.width, sq.height, 1, 3, true);
    } else {
      drawDottedRect(renderer, sq, 2);
    }
  }
}

void drawCircle(const GfxRenderer& renderer, const int cx, const int cy, const int radius) {
  for (const int xDir : {-1, 1}) {
    for (const int yDir : {-1, 1}) renderer.drawArc(radius, cx, cy, xDir, yDir, 1, true);
  }
}

// Clockwise from 12 o'clock through `fraction` of a turn, in the round pen.
void drawProgressArc(const Pen& pen, const int cx, const int cy, const int radius, const float fraction) {
  const int sweep = static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 360.0f + 0.5f);
  if (sweep <= 0) return;
  constexpr int kStartDeg = 270;
  int prevX = cx;
  int prevY = cy - radius;
  pen.dot(prevX, prevY);
  for (int deg = std::min(kArcStepDeg, sweep);; deg = std::min(deg + kArcStepDeg, sweep)) {
    const int x = cx + (radius * cosQ14(kStartDeg + deg) + 8192) / 16384;
    const int y = cy + (radius * sinQ14(kStartDeg + deg) + 8192) / 16384;
    pen.line(prevX, prevY, x, y);
    prevX = x;
    prevY = y;
    if (deg >= sweep) break;
  }
}

// Today's reading against the Daily Goal as a ring, the time read inside it,
// and the current streak underneath. Unframed: the right half of the stats
// card.
void drawTodayCard(const GfxRenderer& renderer, const Rect& r) {
  const int x = r.x + kCardPad;
  const int innerW = r.width - 2 * kCardPad;
  const int titleH = renderer.getLineHeight(kCardTitleFontId);
  renderer.drawText(kCardTitleFontId, x, r.y + kCardPad, tr(STR_TODAY), true);

  const bool tracking = SETTINGS.trackReadingStats;
  const int valueH = renderer.getLineHeight(kCardBodyFontId);
  const int smallH = renderer.getLineHeight(kCardSmallFontId);
  const int footerH = tracking ? valueH : 0;  // one streak line
  const int ringTop = r.y + kCardPad + titleH + kRingGap;
  const int ringBottom = r.y + r.height - kCardPad - footerH - (tracking ? kRingGap : 0);
  const int radius = std::min({kMaxRingRadius, innerW / 2 - kRingPen, (ringBottom - ringTop) / 2 - kRingPen});
  if (radius < 16) return;
  const int cx = r.x + r.width / 2;
  const int cy = ringTop + (ringBottom - ringTop) / 2;

  drawCircle(renderer, cx, cy, radius);
  if (!tracking) {
    const int dashW = renderer.getTextWidth(kCardBodyFontId, "-", EpdFontFamily::BOLD);
    renderer.drawText(kCardBodyFontId, cx - dashW / 2, cy - valueH / 2, "-", true, EpdFontFamily::BOLD);
    return;
  }

  const uint64_t todayMs = READING_STATS.getTodayReadingMs();
  const uint64_t goalMs = SETTINGS.getDailyGoalMs();
  const Pen pen(renderer, kRingPen);
  drawProgressArc(pen, cx, cy, radius, goalMs > 0 ? static_cast<float>(todayMs) / static_cast<float>(goalMs) : 0.0f);

  // "12m" over "/ 30m", centred in the ring.
  const std::string today = ReadingStatsAnalytics::formatDurationHm(todayMs);
  const std::string goal = "/ " + ReadingStatsAnalytics::formatDurationHm(goalMs);
  const int blockTop = cy - (valueH + smallH) / 2;
  renderer.drawText(kCardBodyFontId,
                    cx - renderer.getTextWidth(kCardBodyFontId, today.c_str(), EpdFontFamily::BOLD) / 2, blockTop,
                    today.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawText(kCardSmallFontId, cx - renderer.getTextWidth(kCardSmallFontId, goal.c_str()) / 2, blockTop + valueH,
                    goal.c_str(), true);

  // "4 day streak" as one centred line under the ring.
  char streak[40];
  const uint32_t days = READING_STATS.getCurrentStreakDays();
  if (days == 0) {
    snprintf(streak, sizeof(streak), "%s", tr(STR_STATS_NO_STREAK));
  } else {
    snprintf(streak, sizeof(streak), tr(STR_STATS_DAY_STREAK_FORMAT), static_cast<unsigned>(days));
  }
  const std::string visible = renderer.truncatedText(kCardBodyFontId, streak, innerW, EpdFontFamily::BOLD);
  const int streakW = renderer.getTextWidth(kCardBodyFontId, visible.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(kCardBodyFontId, r.x + (r.width - streakW) / 2, r.y + r.height - kCardPad - valueH, visible.c_str(),
                    true, EpdFontFamily::BOLD);
}

// ---- clipping ----
//
// /My Clippings.txt is the Kindle-format export every saved clipping is
// appended to (ClippingsManager), across all books:
//
//   Title (Author)
//   - Your Highlight on Page 12 | Chapter | Added on ...
//   <blank>
//   text, possibly over several lines
//   ==========
//
// One entry is read straight from a random offset, so a card never loads a
// clippings store (up to ~40 KB for one book).

constexpr char kClippingsPath[] = "/My Clippings.txt";
constexpr char kClipSeparator[] = "==========";
constexpr size_t kClipSeparatorLen = sizeof(kClipSeparator) - 1;
constexpr int kQuoteAttempts = 4;
constexpr size_t kMinQuoteLen = 24;  // shorter picks ("ever be interested") are re-rolled

// Byte reader over an FsFile with a small stack buffer, so the scan is not one
// mutex-taking read() call per byte.
class ByteReader {
 public:
  explicit ByteReader(FsFile& file) : file(file) {}
  // Drops buffered bytes after the caller seeks the file.
  void reset() {
    pos = 0;
    len = 0;
  }
  int next() {
    if (pos >= len) {
      len = file.read(buf, sizeof(buf));
      pos = 0;
      if (len <= 0) {
        len = 0;
        return -1;
      }
    }
    return static_cast<uint8_t>(buf[pos++]);
  }
  // Consumes one line. True when it was exactly the entry separator.
  bool skipLine(bool& eof) {
    size_t n = 0;
    bool matches = true;
    for (int c = next();; c = next()) {
      if (c < 0) {
        eof = true;
        break;
      }
      if (c == '\n') break;
      if (c == '\r') continue;
      matches = matches && n < kClipSeparatorLen && c == kClipSeparator[n];
      ++n;
    }
    return matches && n == kClipSeparatorLen;
  }
  // Reads one line into out (truncated to cap - 1), skipping a UTF-8 BOM.
  bool readLine(char* out, const size_t cap) {
    size_t n = 0;
    int c = next();
    if (c < 0) return false;
    for (; c >= 0 && c != '\n'; c = next()) {
      if (c != '\r' && n + 1 < cap) out[n++] = static_cast<char>(c);
    }
    out[n] = '\0';
    if (n >= 3 && memcmp(out, "\xEF\xBB\xBF", 3) == 0) memmove(out, out + 3, n - 2);
    return true;
  }

 private:
  FsFile& file;
  char buf[96];
  int pos = 0;
  int len = 0;
};

// Text up to the separator, whitespace collapsed to single spaces, cut back
// to a word with an ellipsis when it outgrows cap.
void readClipText(ByteReader& in, char* out, const size_t cap) {
  size_t n = 0;
  size_t lineStart = 0;  // where the current line began in out
  size_t lineLen = 0;
  bool lineIsSeparator = true;
  bool truncated = false;
  for (int c = in.next();; c = in.next()) {
    if (c < 0 || c == '\n') {
      if (lineIsSeparator && lineLen == kClipSeparatorLen) {
        n = lineStart;
        break;
      }
      if (c < 0) break;
      if (n > 0 && out[n - 1] != ' ' && n + 1 < cap) out[n++] = ' ';
      lineStart = n;
      lineLen = 0;
      lineIsSeparator = true;
      continue;
    }
    if (c == '\r') continue;
    lineIsSeparator = lineIsSeparator && lineLen < kClipSeparatorLen && c == kClipSeparator[lineLen];
    ++lineLen;
    const bool space = c == ' ' || c == '\t';
    if (space && (n == 0 || out[n - 1] == ' ')) continue;
    if (n + 1 < cap) {
      out[n++] = space ? ' ' : static_cast<char>(c);
    } else {
      truncated = true;
    }
  }
  while (n > 0 && out[n - 1] == ' ') --n;
  if (truncated) {
    constexpr char kEllipsis[] = "\xE2\x80\xA6";
    size_t cut = std::min(n, cap - sizeof(kEllipsis));
    while (cut > 0 && out[cut - 1] != ' ') --cut;  // a space is never mid-UTF-8
    while (cut > 0 && out[cut - 1] == ' ') --cut;
    n = cut;
    memcpy(out + n, kEllipsis, sizeof(kEllipsis) - 1);
    n += sizeof(kEllipsis) - 1;
  }
  out[n] = '\0';
}

// Picks a random entry: seek somewhere, skip to the next separator (wrapping
// to the start at the end), read that entry. A few tries to avoid a fragment.
// Reads straight into the caller's buffers (the theme's), so the stack holds
// only the reader's 96 B.
bool pickRandomClipping(char* quote, const size_t quoteCap, char* source, const size_t sourceCap) {
  quote[0] = '\0';
  source[0] = '\0';
  if (!Storage.exists(kClippingsPath)) return false;
  FsFile file;
  if (!Storage.openFileForRead("HOME", kClippingsPath, file)) return false;
  const size_t size = file.size();
  ByteReader in(file);
  for (int attempt = 0; attempt < kQuoteAttempts && size > 0; ++attempt) {
    const size_t start = esp_random() % size;
    file.seek(start);
    in.reset();
    if (start > 0) {
      bool eof = false;
      while (!eof && !in.skipLine(eof)) {
      }
      if (eof) {
        file.seek(0);
        in.reset();
      }
    }
    // Skipping can stop on the file's final separator, leaving nothing to
    // read: wrap to the first entry then.
    constexpr int kMaxBlankLines = 3;
    for (int blank = 0; blank < kMaxBlankLines; ++blank) {
      if (!in.readLine(source, sourceCap)) {
        file.seek(0);
        in.reset();
        if (!in.readLine(source, sourceCap)) break;
      }
      if (source[0] != '\0') break;
    }
    bool eof = false;
    in.skipLine(eof);  // "- Your Highlight on ..."
    readClipText(in, quote, quoteCap);
    if (strlen(quote) >= kMinQuoteLen) break;
  }
  file.close();
  if (quote[0] == '\0') source[0] = '\0';
  return quote[0] != '\0';
}

// The quote in curly marks, filling the card, with its book underneath.
void drawClippingCard(const GfxRenderer& renderer, const Rect& r, const char* quote, const char* source) {
  drawCardFrame(renderer, r);
  const int x = r.x + kCardPad;
  const int innerW = r.width - 2 * kCardPad;
  const int innerH = r.height - 2 * kCardPad;
  const int lineH = renderer.getLineHeight(kQuoteFontId);
  const int smallH = renderer.getLineHeight(kCardSmallFontId);
  if (quote[0] == '\0') {
    const char* empty = tr(STR_NO_CLIPPINGS);
    for (const auto& line : renderer.wrappedText(kCardSmallFontId, empty, innerW, 2)) {
      const int w = renderer.getTextWidth(kCardSmallFontId, line.c_str());
      renderer.drawText(kCardSmallFontId, r.x + (r.width - w) / 2, r.y + (r.height - smallH) / 2, line.c_str(), true);
      break;
    }
    return;
  }

  const bool hasSource = source[0] != '\0';
  const int sourceH = hasSource ? smallH + 6 : 0;
  const int maxLines = std::max(1, (innerH - sourceH) / lineH);
  char marked[LicoriceTheme::kQuoteMax + 8];
  snprintf(marked, sizeof(marked), "\xE2\x80\x9C%s\xE2\x80\x9D", quote);
  int y = r.y + kCardPad;
  for (const auto& line : renderer.wrappedText(kQuoteFontId, marked, innerW, maxLines)) {
    renderer.drawText(kQuoteFontId, x, y, line.c_str(), true);
    y += lineH;
  }
  if (hasSource) {
    char dashed[LicoriceTheme::kQuoteSourceMax + 8];
    snprintf(dashed, sizeof(dashed), "\xE2\x80\x94 %s", source);
    const std::string fitted = renderer.truncatedText(kCardSmallFontId, dashed, innerW, EpdFontFamily::BOLD);
    renderer.drawText(kCardSmallFontId, x, r.y + r.height - kCardPad - smallH, fitted.c_str(), true,
                      EpdFontFamily::BOLD);
  }
}

// ---- folder ----

// GfxRenderer::drawIcon in portrait blits straight into the panel buffer, whose
// rows are bytes, so the icon's y snaps down to a multiple of 8 -- up to 7 px
// off centre in a tile. This places each ink pixel exactly instead, decoding
// the same pre-rotated layout drawIcon's other-orientation path does. A 32 px
// icon is ~1000 pixel writes, four of them per frame.
void drawIconExact(const GfxRenderer& renderer, const uint8_t* bitmap, const int x, const int y, const int size) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    const uint8_t* src = bitmap + row * rowBytes;
    for (int col = 0; col < size; ++col) {
      if (!(src[col >> 3] & (0x80 >> (col & 7)))) renderer.drawPixel(x + size - 1 - row, y + col, true);
    }
  }
}

// ---- book bar ----

const char* bookTitle(const RecentBook& book) { return book.title.empty() ? book.path.c_str() : book.title.c_str(); }

// "12% · 1h 4m", from the store's per-book time when tracking is on. Empty for
// a book with neither.
void formatBookProgress(const RecentBook& book, char* buf, const size_t len) {
  buf[0] = '\0';
  std::string time;
  if (SETTINGS.trackReadingStats) {
    const ReadingBookStats* stats = READING_STATS.findMatchingBookForPath(book.path, book.title, book.author);
    if (stats != nullptr && stats->totalReadingMs >= 60000) {
      time = ReadingStatsAnalytics::formatDurationHm(stats->totalReadingMs);
    }
  }
  const int percent = book.progressPercent;
  if (percent >= 0 && !time.empty()) {
    snprintf(buf, len, "%d%% \xC2\xB7 %s", percent, time.c_str());
  } else if (percent >= 0) {
    snprintf(buf, len, "%d%%", percent);
  } else if (!time.empty()) {
    snprintf(buf, len, "%s", time.c_str());
  }
}

// The book row: a box of two solid rules, with title / author / progress and a
// progress bar under them inside it.
// The cover stands at the left, set in from the edge and taller than the box,
// so it pokes out above and below; the rules break around it.
constexpr int kBookRowPad = 8;       // text inside the box, above and below
constexpr int kCoverPoke = 10;       // cover overhangs the box by this much top and bottom
constexpr int kBookSideMargin = 14;  // cover from the left edge, text from the right
constexpr int kRuleGap = 6;          // rules stop this far short of the cover
constexpr int kProgressBarH = 8;     // outlined bar; the fill sits inset inside it
constexpr int kProgressInset = 2;
constexpr int kProgressGap = 6;  // text to bar

int bookTextBlockHeight(const GfxRenderer& renderer) {
  return renderer.getLineHeight(kBookTitleFontId) + 2 * renderer.getLineHeight(kBookMetaFontId);
}

int bookBoxHeight(const GfxRenderer& renderer) {
  return 1 + kBookRowPad + bookTextBlockHeight(renderer) + kProgressGap + kProgressBarH + kBookRowPad + 1;
}

int bookRowHeight(const GfxRenderer& renderer) { return bookBoxHeight(renderer) + 2 * kCoverPoke; }

// A horizontal band across [x0, x1) that skips [skipL, skipR).
void fillAround(const GfxRenderer& renderer, const int x0, const int x1, const int y, const int h, const int skipL,
                const int skipR) {
  if (skipL > x0) renderer.fillRect(x0, y, std::min(skipL, x1) - x0, h, true);
  if (skipR < x1) renderer.fillRect(std::max(skipR, x0), y, x1 - std::max(skipR, x0), h, true);
}

// Returns the cover's rect (the selection outline goes round it), or an empty
// rect with no book.
Rect drawBookBar(const GfxRenderer& renderer, const Rect& r, const RecentBook* book, const int thumbHeight) {
  const int boxTop = r.y + kCoverPoke;
  const int boxBottom = r.y + r.height - kCoverPoke;  // last row of the bottom rule + 1
  const int barTop = boxBottom - 1 - kBookRowPad - kProgressBarH;
  const int x1 = r.x + r.width;
  if (book == nullptr) {
    renderer.fillRect(r.x, boxTop, r.width, 1, true);
    renderer.fillRect(r.x, boxBottom - 1, r.width, 1, true);
    return Rect{};
  }

  const Rect box{r.x + kBookSideMargin, r.y, (r.height * 2) / 3, r.height};
  const CoverCache::CoverPlacement place{box, /*bottomAlign=*/false, /*centerX=*/false, "licbar", 4};
  Rect drawn;
  if (!CoverCache::drawCoverCached(renderer, *book, place, thumbHeight, drawn)) {
    drawn = box;
    renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 1, 4, true);
  }
  const int skipL = drawn.x - kRuleGap;
  const int skipR = drawn.x + drawn.width + kRuleGap;
  fillAround(renderer, r.x, x1, boxTop, 1, skipL, skipR);
  fillAround(renderer, r.x, x1, boxBottom - 1, 1, skipL, skipR);

  const int textX = drawn.x + drawn.width + kTextGap;
  const int textW = x1 - kBookSideMargin - textX;
  if (textW < 40) return drawn;
  // The bar runs under the text, from its left edge to the right margin.
  renderer.drawRoundedRect(textX, barTop, textW, kProgressBarH, 1, kProgressBarH / 2, true);
  if (book->progressPercent > 0) {
    const int innerW = textW - 2 * kProgressInset;
    const int innerH = kProgressBarH - 2 * kProgressInset;
    const int fillW = innerW * std::min(static_cast<int>(book->progressPercent), 100) / 100;
    renderer.fillRoundedRect(textX + kProgressInset, barTop + kProgressInset, std::max(innerH, fillW), innerH,
                             innerH / 2, Color::Black);
  }

  const int titleH = renderer.getLineHeight(kBookTitleFontId);
  const int metaH = renderer.getLineHeight(kBookMetaFontId);
  int y = boxTop + 1 + kBookRowPad;
  const std::string title = renderer.truncatedText(kBookTitleFontId, bookTitle(*book), textW);
  renderer.drawText(kBookTitleFontId, textX, y, title.c_str(), true);
  y += titleH;
  if (!book->author.empty()) {
    const std::string author = renderer.truncatedText(kBookMetaFontId, book->author.c_str(), textW);
    renderer.drawText(kBookMetaFontId, textX, y, author.c_str(), true);
  }
  y += metaH;
  char progress[48];
  formatBookProgress(*book, progress, sizeof(progress));
  if (progress[0] != '\0') renderer.drawText(kBookMetaFontId, textX, y, progress, true);
  return drawn;
}

}  // namespace

void LicoriceTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                        const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                        bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  (void)storeCoverBuffer;
  const ThemeMetrics& metrics = themeMetrics();
  const Layout l = computeLayout(renderer, rect, metrics);
  // No snapshot (see LicoriceMetrics::values): every frame is drawn whole, so
  // tell HomeActivity there is nothing to restore.
  coverRendered = false;
  coverBufferStored = false;

  bookBar = l.bar;
  statsCard = l.stats;
  folderCard = l.folder;
  clipCard = l.clip;

  tm now{};
  uint32_t epoch = 0;
  const bool hasTime = localNow(now, epoch);
  drawClock(renderer, l.clock, l.digitH, hasTime, now);
  // One long card, the Statistics button: This month | Today.
  drawCardFrame(renderer, l.stats);
  renderer.fillRect(l.today.x, l.stats.y + kCardPad, 1, l.stats.height - 2 * kCardPad, true);
  drawMonthCard(renderer, l.month, hasTime, now);
  drawTodayCard(renderer, l.today);

  if (quoteStale) {
    pickRandomClipping(quote, sizeof(quote), quoteSource, sizeof(quoteSource));
    quoteStale = false;
  }
  drawClippingCard(renderer, l.clip, quote, quoteSource);

  const RecentBook* book = recentBooks.empty() ? nullptr : &recentBooks[0];
  const Rect cover = drawBookBar(renderer, l.bar, book, metrics.homeCoverHeight);
  if (selectorIndex == 0 && cover.width > 0) {
    renderer.drawRoundedRect(cover.x - 4, cover.y - 4, cover.width + 8, cover.height + 8, 2, kCoverRadius + 3, true);
  }
}

// The menu is drawn as the folder (every item but Stats, as square tiles) and
// the month card's selection (Stats). Runs after drawRecentBookCover, which
// recorded both cards.
void LicoriceTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, const int buttonCount, const int selectedIndex,
                                   const std::function<std::string(int index)>& buttonLabel,
                                   const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rect;
  statsMenuIndex = -1;
  clipMenuIndex = -1;
  for (int& index : folderMenuIndex) index = -1;
  if (buttonCount <= 0 || folderCard.width <= 0) return;

  int placed = 0;
  for (int i = 0; i < buttonCount; ++i) {
    if (statsMenuIndex < 0 && rowIcon != nullptr && rowIcon(i) == UIIcon::Stats) {
      statsMenuIndex = i;
    } else if (clipMenuIndex < 0 && rowIcon != nullptr && rowIcon(i) == UIIcon::Clippings) {
      clipMenuIndex = i;
    } else if (placed < kFolderSlots) {
      folderMenuIndex[placed++] = i;
    }
  }
  if (statsMenuIndex >= 0 && selectedIndex == statsMenuIndex) drawSelectedFrame(renderer, statsCard);
  if (clipMenuIndex >= 0 && selectedIndex == clipMenuIndex) drawSelectedFrame(renderer, clipCard);

  // No frame round the folder: the four tiles fill its place as live tiles,
  // their outer edges flush with the cards around them.
  const Rect& f = folderCard;
  const int tileW = std::max(1, (f.width - kCardGap) / 2);
  const int tileH = std::max(1, (f.height - kCardGap) / 2);

  for (int k = 0; k < kFolderSlots; ++k) {
    const int col = k % 2;
    const int row = k / 2;
    // The right column / bottom row take any odd pixel, so the grid ends
    // exactly on the folder's edges.
    const int x = f.x + col * (tileW + kCardGap);
    const int y = f.y + row * (tileH + kCardGap);
    const Rect t{x, y, col == 0 ? tileW : f.x + f.width - x, row == 0 ? tileH : f.y + f.height - y};
    folderTiles[k] = t;
    const int i = folderMenuIndex[k];
    if (i < 0) continue;
    // drawIcon only sets black pixels, so selection is a grey plate under the
    // icon rather than an inverted tile.
    if (i == selectedIndex) renderer.fillRoundedRect(t.x, t.y, t.width, t.height, kCardRadius, Color::LightGray);
    renderer.drawRoundedRect(t.x, t.y, t.width, t.height, kCardStroke, kCardRadius, true);
    const uint8_t* icon = rowIcon != nullptr ? menuIconBitmap(rowIcon(i)) : nullptr;
    if (icon != nullptr) {
      drawIconExact(renderer, icon, t.x + (t.width - kMenuIconSize) / 2, t.y + (t.height - kMenuIconSize) / 2,
                    kMenuIconSize);
    }
  }
}
