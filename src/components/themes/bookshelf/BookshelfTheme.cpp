#include "BookshelfTheme.h"

#include <Bitmap.h>
#include <BitmapHelpers.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/bookfusion24.h"
#include "components/icons/clippings.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/transfer.h"
#include "components/themes/CoverCache.h"
#include "fontIds.h"

const ThemeMetrics& BookshelfTheme::themeMetrics() const {
#if FREEINK_CAP_TOUCH
  // See BaseTheme::themeMetrics().
  return CrossPointSettings::FULL_TOUCH_UI ? BookshelfMetrics::values : BookshelfMetrics::noActionBarValues;
#else
  return BookshelfMetrics::values;
#endif
}

namespace {

constexpr int kLabelFontId = SMALL_FONT_ID;
constexpr int kTitleFontId = UI_12_FONT_ID;
constexpr int kAuthorFontId = UI_10_FONT_ID;

constexpr int kContentInset = 20;
constexpr int kTopInset = 12;

// Hero card: cover on the left, text column to its right.
constexpr int kMaxHeroCoverH = 340;
constexpr int kMinHeroCoverH = 90;
constexpr int kHeroCornerRadius = 8;
constexpr int kHeroTextGap = 16;
constexpr int kHeroLabelGap = 8;
constexpr int kHeroTitleMaxLines = 4;
constexpr int kHeroAuthorMaxLines = 2;
constexpr int kHeroBlurbGap = 8;
constexpr int kBlurbFontId = SMALL_FONT_ID;
// How much of the description sidecar to read for the card: more than the
// column beside the cover plus the full-width lines under it can show in
// portrait, and still well short of Epub::MAX_DESCRIPTION_BYTES (4 KB).
constexpr size_t kBlurbReadBytes = 1536;
constexpr int kProgressBarH = 6;
constexpr int kProgressTextGap = 6;

// Shelf: books stand bottom-aligned on a plank, packed from the left like a
// real shelf.
constexpr int kSectionTopGap = 18;
constexpr int kSectionLabelGap = 8;
constexpr int kBookGap = 8;
constexpr int kMaxShelfCoverH = 108;  // shelf (and spine) height; covers usually stand shorter
constexpr int kMinShelfCoverH = 48;
constexpr int kShelfCornerRadius = 4;
constexpr int kPlankEdgeH = 3;  // solid top edge the books stand on
constexpr int kPlankFaceH = 9;  // dithered shelf front
constexpr int kPlankH = kPlankEdgeH + kPlankFaceH;
constexpr int kPlankOverhang = 6;
constexpr int kShelfGridGap = 16;
constexpr int kSelectOutset = 3;
constexpr int kCaretH = 6;

// Spine-out books: an upright strip the height of a full cover with the title
// running up it. Every coverless book stands this way, plus roughly one in
// three of the rest (picked by a hash of the path, so a book keeps its pose
// between visits) to break up the row the way a real shelf looks.
constexpr int kSpineW = 30;
constexpr int kSpineTextPad = 8;
constexpr int kSpineBandInset = 7;
constexpr uint32_t kSpineOneIn = 3;
constexpr int kSpineFontId = SMALL_FONT_ID;

// Button grid.
constexpr int kGridCellH = 52;
constexpr int kGridGap = 8;
constexpr int kGridBottomGap = 10;
constexpr int kGridCornerRadius = 10;
constexpr int kGridIconSize = 32;  // must match the bitmaps; drawIcon does not scale
constexpr int kGridIconPad = 14;
constexpr int kGridLabelFontId = UI_10_FONT_ID;
constexpr int kGridMaxCells = 6;  // the whole home menu, Library first

constexpr int kPlaceholderIconSize = 32;
constexpr int kMaxShelfSlots = BookshelfMetrics::values.homeRecentBooksCount;
// Shelf positions that may show a cover. Every book past this position always
// stands spine-on, which is what lets a sixth book fit in a ~30 px strip.
constexpr int kMaxFaceOut = 5;
static_assert(kMaxShelfSlots >= kMaxFaceOut, "the shelf holds at least kMaxFaceOut books");

// ---- state shared between draw and hit test ----
//
// The hit tests are handed no renderer and no selector index, and
// drawButtonMenu is handed no shelf geometry, so what they need is recorded
// here as it is drawn. Single-threaded: only the UI task draws or hit-tests
// the home screen.

// The book the hero card previews. Follows the selector while it is on the
// shelf and holds still while the selector is in the grid, so
// leaving the shelf does not snap the card back to the first book.
int focusedIdx = 0;

Rect drawnHero{0, 0, 0, 0};
Rect drawnSlots[kMaxShelfSlots];
int drawnSlotCount = 0;

// Each book's rect as last painted, cover or spine. It depends on the cover's
// aspect, and the selection outline needs it on the frames that reuse the
// snapshot instead of re-reading the thumbnail.
Rect drawnArtwork[kMaxShelfSlots];

// Set when drawRecentBookCover lays out a shelf, cleared by drawButtonMenu:
// a drawButtonMenu call with no shelf behind it is not the home screen.
bool shelfLaidOut = false;

struct GridLayout {
  int left;
  int top;
  int cols;
  int rows;
  int cellW;
};
GridLayout drawnGrid{0, 0, 0, 0, 0};

// Two columns of three rows in portrait; one row of six in landscape, where a second row would
// leave no height for the card.
GridLayout computeGrid(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);

  GridLayout g{};
  const bool wide = renderer.getScreenWidth() > renderer.getScreenHeight();
  g.cols = wide ? 6 : 2;
  g.rows = kGridMaxCells / g.cols;
  g.left = kContentInset + left;
  const int width = renderer.getScreenWidth() - kContentInset - right - g.left;
  g.cellW = std::max(1, (width - (g.cols - 1) * kGridGap) / g.cols);
  const int gridH = g.rows * kGridCellH + (g.rows - 1) * kGridGap;
  g.top = renderer.getScreenHeight() - metrics.buttonHintsHeight - kGridBottomGap - gridH;
  return g;
}

struct Layout {
  int left;
  int right;
  Rect heroCover;  // box; the artwork letterboxes inside it
  int heroTextX;
  int heroTextW;
  int heroBottom;  // the card's text may run down to here, under the cover too
  bool showHero;
  int sectionLabelY;
  int plankY;  // books stand on this line
  int slotCount;
  int shelfCoverMaxW;
  int shelfCoverMaxH;
};

// Everything the draw helpers need, derived once. The shelf is sized first and
// anchored just above the grid; the hero card gets whatever height is left, so
// a short frame (landscape, X4 Pro gesture mode) shrinks the card first and
// drops it entirely before it would overlap the shelf.
Layout computeLayout(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics, const int bookCount) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);

  Layout l;  // every field is assigned below; Rect's explicit ctor rules out l{}
  l.left = kContentInset + left;
  l.right = renderer.getScreenWidth() - kContentInset - right;
  const int contentW = std::max(1, l.right - l.left);
  const int contentTop = rect.y + kTopInset;
  const int gridTop = computeGrid(renderer, metrics).top;

  // Worst case the shelf holds kMaxFaceOut covers plus the forced spines.
  // Each cover gets an equal share of what those leave,
  // sized for a full shelf even when fewer books exist, so a cover keeps its
  // size as the recents list grows.
  l.slotCount = std::min(bookCount, kMaxShelfSlots);
  const int spinesW = (kMaxShelfSlots - kMaxFaceOut) * (kSpineW + kBookGap);
  const int slotW = (contentW - spinesW) / kMaxFaceOut;
  l.shelfCoverMaxW = std::max(1, slotW - kBookGap);

  const int available = gridTop - kShelfGridGap - contentTop;
  // Shelf height, deliberately not derived from the cover width: spines stand
  // this tall so their titles get room, while a cover fits inside
  // shelfCoverMaxW x shelfCoverMaxH at its own aspect and so usually stands
  // shorter, the way mixed books do on a real shelf.
  l.shelfCoverMaxH = std::clamp(std::min(kMaxShelfCoverH, available / 3), kMinShelfCoverH, kMaxShelfCoverH);

  l.plankY = gridTop - kShelfGridGap - kPlankH;
  l.sectionLabelY = l.plankY - l.shelfCoverMaxH - kSectionLabelGap - renderer.getLineHeight(kLabelFontId);

  const int heroBottom = l.sectionLabelY - kSectionTopGap;
  const int heroH = std::min({kMaxHeroCoverH, metrics.homeCoverHeight, heroBottom - contentTop});
  l.showHero = heroH >= kMinHeroCoverH;
  const int heroW = std::max(1, (heroH * 2) / 3);
  l.heroCover = Rect{l.left, contentTop, heroW, std::max(0, heroH)};
  l.heroTextX = l.left + heroW + kHeroTextGap;
  l.heroTextW = std::max(1, l.right - l.heroTextX);
  l.heroBottom = std::max(l.heroCover.y + l.heroCover.height, heroBottom);
  return l;
}

// ---- covers ----

void drawPlaceholder(const GfxRenderer& renderer, const Rect& r, const int radius) {
  renderer.fillRoundedRect(r.x, r.y + r.height / 3, r.width, 2 * r.height / 3, radius, /*roundTopLeft=*/false,
                           /*roundTopRight=*/false, /*roundBottomLeft=*/true, /*roundBottomRight=*/true, Color::Black);
  if (r.width > kPlaceholderIconSize + 8 && r.height / 3 > kPlaceholderIconSize / 2) {
    renderer.drawIcon(CoverIcon, r.x + (r.width - kPlaceholderIconSize) / 2, r.y + r.height / 3 + 10,
                      kPlaceholderIconSize, kPlaceholderIconSize);
  }
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, radius, true);
}

// BookFusion-linked books get the same corner badge as the Lyra themes. iconY
// is snapped to a multiple of 8 because drawIcon truncates the display-y with
// an integer divide by 8; an unaligned y shifts the icon against its white plate.
void drawBookFusionBadge(const GfxRenderer& renderer, const Rect& cover) {
  constexpr int kIconSize = 24;
  constexpr int kPadding = 4;
  constexpr int kMargin = 6;
  constexpr int kBadgeSize = kIconSize + 2 * kPadding;
  if (cover.width < kBadgeSize + 2 * kMargin || cover.height < kBadgeSize + 2 * kMargin) return;
  const int iconY = ((cover.y + cover.height - kMargin - kPadding - kIconSize) / 8) * 8;
  const int badgeX = cover.x + kMargin;
  renderer.fillRect(badgeX, iconY - kPadding, kBadgeSize, kBadgeSize, false);
  renderer.drawIcon(BookFusion24Icon, badgeX + kPadding, iconY, kIconSize, kIconSize);
}

// ---- shelf ----

// FNV-1a: a stable per-book value for the spine pick and style. std::hash
// would do but makes no promise to stay the same across toolchains.
uint32_t pathHash(const std::string& path) {
  uint32_t h = 2166136261u;
  for (const char c : path) {
    h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
  }
  return h;
}

// Stands book spine-out in r: a dark or light spine by hash, a band near each
// end, and the title reading bottom-to-top (drawTextRotated90CW's direction,
// which is also how continental European spines run).
void drawSpine(const GfxRenderer& renderer, const RecentBook& book, const Rect& r, const uint32_t hash) {
  const bool dark = ((hash >> 8) & 1u) != 0;
  if (dark) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, 2, Color::Black);
  } else {
    renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, 2, true);
  }
  // Two thin bands, the way a cloth spine is tooled near the head and tail.
  const bool ink = !dark;
  renderer.drawLine(r.x + 3, r.y + kSpineBandInset, r.x + r.width - 4, r.y + kSpineBandInset, ink);
  renderer.drawLine(r.x + 3, r.y + r.height - 1 - kSpineBandInset, r.x + r.width - 4,
                    r.y + r.height - 1 - kSpineBandInset, ink);

  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int maxLen = r.height - 2 * (kSpineBandInset + kSpineTextPad);
  if (maxLen <= 0) return;
  const std::string visible = renderer.truncatedText(kSpineFontId, title, maxLen, EpdFontFamily::BOLD);
  const int textW = renderer.getTextWidth(kSpineFontId, visible.c_str(), EpdFontFamily::BOLD);
  const int textH = renderer.getTextHeight(kSpineFontId);
  // Same anchor maths as BaseTheme's rotated side-button labels: x is the
  // glyphs' left edge, y is where the text starts and it climbs from there.
  renderer.drawTextRotated90CW(kSpineFontId, r.x + (r.width - textH) / 2, r.y + (r.height + textW) / 2, visible.c_str(),
                               ink, EpdFontFamily::BOLD);
}

// A ribbon marker hanging over the head of a pinned book, drawn in both inks
// so it reads on a dark spine and a light cover alike.
void drawPinRibbon(const GfxRenderer& renderer, const Rect& book) {
  constexpr int kRibbonW = 7;
  constexpr int kRibbonH = 14;
  const int x = book.x + book.width - kRibbonW - 3;
  const int y = book.y;
  if (x <= book.x) return;
  renderer.fillRect(x - 1, y, kRibbonW + 2, kRibbonH + 1, false);
  renderer.fillRect(x, y, kRibbonW, kRibbonH, true);
  // Notch: a white wedge cut up into the ribbon's tail.
  const int cx = x + kRibbonW / 2;
  for (int row = 0; row < kRibbonW / 2 + 1; ++row) {
    renderer.drawLine(cx - row, y + kRibbonH - row, cx + row, y + kRibbonH - row, false);
  }
}

using CoverCache::CoverPlacement;
using CoverCache::drawCoverCached;

// A shelf cover standing on the plank with its left edge at x. False when
// there is no readable thumbnail, so the caller stands the book spine-out.
bool drawShelfCover(const GfxRenderer& renderer, const RecentBook& book, const int x, const int plankY, const int maxW,
                    const int maxH, const int thumbHeight, Rect& drawnOut) {
  const CoverPlacement place{Rect{x, plankY - maxH, maxW, maxH}, /*bottomAlign=*/true, /*centerX=*/false, "shelf",
                             kShelfCornerRadius};
  return drawCoverCached(renderer, book, place, thumbHeight, drawnOut);
}

// The hero card's cover, top-aligned and centred in box, or a 2:3 placeholder
// when the book has no artwork. Returns the rect painted.
Rect drawHeroCover(const GfxRenderer& renderer, const RecentBook& book, const Rect& box, const int thumbHeight) {
  const CoverPlacement place{box, /*bottomAlign=*/false, /*centerX=*/true, "hero", kHeroCornerRadius};
  Rect drawn;
  if (drawCoverCached(renderer, book, place, thumbHeight, drawn)) return drawn;
  const int w = std::min(box.width, (box.height * 2) / 3);
  const Rect placeholder{box.x + (box.width - w) / 2, box.y, w, box.height};
  drawPlaceholder(renderer, placeholder, kHeroCornerRadius);
  return placeholder;
}

void drawPlank(const GfxRenderer& renderer, const Layout& l) {
  const int x = std::max(0, l.left - kPlankOverhang);
  const int w = std::min(renderer.getScreenWidth(), l.right + kPlankOverhang) - x;
  renderer.fillRect(x, l.plankY, w, kPlankEdgeH, true);
  renderer.fillRectDither(x, l.plankY + kPlankEdgeH, w, kPlankFaceH, Color::LightGray);
  renderer.drawLine(x, l.plankY + kPlankH - 1, x + w - 1, l.plankY + kPlankH - 1, true);
}

void drawShelf(const GfxRenderer& renderer, const Layout& l, const std::vector<RecentBook>& books,
               const int thumbHeight) {
  renderer.drawText(kLabelFontId, l.left, l.sectionLabelY, tr(STR_MENU_RECENT_BOOKS), true, EpdFontFamily::BOLD);

  // Books pack from the left, so a spine takes only its own width and the gap
  // at the end varies like a real shelf. Each book is at most shelfCoverMaxW
  // wide and only the first kMaxFaceOut positions can show a cover, so the
  // row never overruns the shelf.
  int x = l.left + kBookGap / 2;
  for (int i = 0; i < l.slotCount; ++i) {
    const uint32_t hash = pathHash(books[i].path);
    Rect drawn;
    const bool asSpine = i >= kMaxFaceOut || (hash % kSpineOneIn) == 0;
    if (asSpine ||
        !drawShelfCover(renderer, books[i], x, l.plankY, l.shelfCoverMaxW, l.shelfCoverMaxH, thumbHeight, drawn)) {
      drawn = Rect{x, l.plankY - l.shelfCoverMaxH, kSpineW, l.shelfCoverMaxH};
      drawSpine(renderer, books[i], drawn, hash);
    }
    if (books[i].pinned) drawPinRibbon(renderer, drawn);
    drawnArtwork[i] = drawn;
    // Hit area: the book's own column, full shelf height down to the plank,
    // widened by half a gap each side so taps between books still land.
    drawnSlots[i] = Rect{drawn.x - kBookGap / 2, l.plankY - l.shelfCoverMaxH, drawn.width + kBookGap, l.shelfCoverMaxH};
    x += drawn.width + kBookGap;
  }
  drawnSlotCount = l.slotCount;
  drawPlank(renderer, l);
}

// Outline around a book plus a caret on the plank
// front, so the selection reads even on a cover that is mostly black.
void drawShelfSelection(const GfxRenderer& renderer, const Rect& a, const int plankY) {
  renderer.drawRoundedRect(a.x - kSelectOutset, a.y - kSelectOutset, a.width + 2 * kSelectOutset,
                           a.height + 2 * kSelectOutset, 2, kShelfCornerRadius + kSelectOutset, true);
  const int cx = a.x + a.width / 2;
  const int caretTop = plankY + kPlankEdgeH + 1;
  for (int row = 0; row < kCaretH && row < kPlankFaceH - 1; ++row) {
    renderer.drawLine(cx - row, caretTop + row, cx + row, caretTop + row, true);
  }
}

// ---- button grid ----

// Our 32 px icon set, for the icons the home grid can show.
const uint8_t* gridIcon(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Folder:
      return FolderIcon;
    case UIIcon::Book:
      return BookIcon;
    case UIIcon::Recent:
      return RecentIcon;
    case UIIcon::Settings:
      return Settings2Icon;
    case UIIcon::Transfer:
      return TransferIcon;
    case UIIcon::Library:
      return LibraryIcon;
    case UIIcon::Stats:
      return StatsIcon;
    case UIIcon::Clippings:
      return ClippingsIcon;
    default:
      return nullptr;
  }
}

Rect gridCell(const GridLayout& g, const int cell) {
  const int col = cell % g.cols;
  const int row = cell / g.cols;
  return Rect{g.left + col * (g.cellW + kGridGap), g.top + row * (kGridCellH + kGridGap), g.cellW, kGridCellH};
}

// A selected cell is LightGray with a heavy border rather than inverted,
// because drawIcon is transparent-only and cannot knock an icon out in white.
void drawGridCell(const GfxRenderer& renderer, const Rect& r, const char* label, const uint8_t* icon,
                  const bool selected) {
  if (selected) {
    renderer.fillRoundedRect(r.x, r.y, r.width, r.height, kGridCornerRadius, Color::LightGray);
    renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 3, kGridCornerRadius, true);
  } else {
    renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, kGridCornerRadius, true);
  }

  int textX = r.x + kGridIconPad;
  if (icon != nullptr) {
    renderer.drawIcon(icon, textX, r.y + (r.height - kGridIconSize) / 2, kGridIconSize, kGridIconSize);
    textX += kGridIconSize + kGridIconPad / 2;
  }
  const int textMaxW = r.x + r.width - kGridIconPad / 2 - textX;
  if (textMaxW <= 0) return;
  const std::string visible = renderer.truncatedText(kGridLabelFontId, label, textMaxW);
  renderer.drawText(kGridLabelFontId, textX, r.y + (r.height - renderer.getLineHeight(kGridLabelFontId)) / 2,
                    visible.c_str(), true);
}

// ---- hero card ----

// Progress percentage (or Not started / Book Finished) with its bar above
// it, sitting on bottomY and spanning x..x+w.
void drawProgress(const GfxRenderer& renderer, const int x, const int w, const int bottomY, const int progressPercent) {
  char buf[40];
  if (progressPercent < 0 || progressPercent == 0) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BOOK_INFO_NOT_STARTED));
  } else if (progressPercent >= 100) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BOOK_FINISHED));
  } else {
    snprintf(buf, sizeof(buf), "%d%%", progressPercent);
  }
  const int textY = bottomY - renderer.getLineHeight(kLabelFontId);
  renderer.drawText(kLabelFontId, x, textY, buf, true);

  if (progressPercent <= 0) return;
  const int barY = textY - kProgressTextGap - kProgressBarH;
  const int pct = std::min(100, progressPercent);
  renderer.drawRoundedRect(x, barY, w, kProgressBarH, 1, kProgressBarH / 2, true);
  const int fillW = (w * pct) / 100;
  if (fillW >= kProgressBarH) {
    renderer.fillRoundedRect(x, barY, fillW, kProgressBarH, kProgressBarH / 2, Color::Black);
  }
}

// Where the card's text goes: a column beside the cover until the text clears
// the cover's bottom edge, then the full card width underneath it.
struct TextFlow {
  int y;       // top of the next line
  int splitY;  // a line starting at or below this may use the full width
  int colX;
  int colW;
  int fullX;
  int fullW;
  int bottomY;  // no line may end below this
  int x() const { return y < splitY ? colX : fullX; }
  int w() const { return y < splitY ? colW : fullW; }
};

// Greedy word wrap of text into the flow, at most maxLines lines. When the
// text outruns the room, the last line that fits is ellipsised. Breaks only at
// spaces, so a multi-byte UTF-8 sequence is never split; a single word wider
// than the line is truncated instead. Advances flow.y past what it drew; the
// next call checks for room itself, so running out needs no signal.
//
// Linear in the text: each word is measured once and a line's width is the
// running sum of its words plus the inter-word advances. Re-measuring the
// growing line after every word was quadratic, and on a 1.5 KB blurb that was
// most of the lag moving between books on the C3. The sum ignores kerning
// across the spaces, hence the small slack.
void flowText(const GfxRenderer& renderer, TextFlow& flow, const int fontId, const char* text, const int maxLines,
              const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  constexpr int kKerningSlack = 2;
  const int lineH = renderer.getLineHeight(fontId);
  const int spaceW = renderer.getTextAdvanceX(fontId, " ", style);
  std::string buf;  // one reused buffer for measuring and drawing
  const char* p = text;
  for (int lines = 0; lines < maxLines; ++lines) {
    while (*p == ' ') ++p;
    if (*p == '\0' || flow.y + lineH > flow.bottomY) return;

    const int x = flow.x();
    const int w = flow.w() - kKerningSlack;
    const bool lastLine = lines + 1 >= maxLines || flow.y + 2 * lineH > flow.bottomY;

    // Longest run of whole words from p that fits in w; overflowEnd is the end
    // of the first word that did not, for the ellipsis case.
    const char* end = nullptr;
    const char* overflowEnd = nullptr;
    int lineW = 0;
    const char* scan = p;
    while (true) {
      const char* wordStart = scan;
      while (*wordStart == ' ') ++wordStart;
      if (*wordStart == '\0') break;
      const char* wordEnd = wordStart;
      while (*wordEnd != '\0' && *wordEnd != ' ') ++wordEnd;
      buf.assign(wordStart, wordEnd - wordStart);
      const int candidateW = (end != nullptr ? lineW + spaceW : 0) + renderer.getTextWidth(fontId, buf.c_str(), style);
      if (candidateW > w) {
        overflowEnd = wordEnd;
        break;
      }
      lineW = candidateW;
      end = wordEnd;
      scan = wordEnd;
    }

    if (lastLine && overflowEnd != nullptr) {
      // More text than room: this line plus the word that did not fit,
      // truncated to an ellipsis. Only that much is handed to truncatedText,
      // never the whole remaining blurb.
      buf.assign(p, overflowEnd - p);
      const std::string cut = renderer.truncatedText(fontId, buf.c_str(), w, style);
      renderer.drawText(fontId, x, flow.y, cut.c_str(), true, style);
      flow.y += lineH;
      return;
    }
    if (end == nullptr) {
      // A single word wider than the line.
      const char* wordEnd = p;
      while (*wordEnd != '\0' && *wordEnd != ' ') ++wordEnd;
      buf.assign(p, wordEnd - p);
      const std::string cut = renderer.truncatedText(fontId, buf.c_str(), w, style);
      renderer.drawText(fontId, x, flow.y, cut.c_str(), true, style);
      p = wordEnd;
    } else {
      buf.assign(p, end - p);
      renderer.drawText(fontId, x, flow.y, buf.c_str(), true, style);
      p = end;
    }
    flow.y += lineH;
  }
}

// Flows as much of the book's description (the plain-text sidecar the metadata
// pass writes) as fits. Only the first kBlurbReadBytes are read -- the card
// never shows the whole 4 KB. Heap, not stack, because the buffer is past the
// 256 B stack budget; it is freed before returning.
void drawBlurb(const GfxRenderer& renderer, TextFlow& flow, const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path)) return;
  if (flow.y + renderer.getLineHeight(kBlurbFontId) > flow.bottomY) return;
  // Constructing an Epub only derives its cache path; nothing is loaded.
  const Epub epub(book.path, "/.crosspoint");
  const std::string descPath = epub.getDescriptionPath();
  if (!Storage.exists(descPath.c_str())) return;
  FsFile file;
  if (!Storage.openFileForRead("HOME", descPath, file)) return;

  const size_t fileSize = static_cast<size_t>(file.size());
  const size_t toRead = std::min<size_t>(fileSize, kBlurbReadBytes);
  auto* buf = static_cast<char*>(malloc(toRead + 1));
  if (buf == nullptr) {
    LOG_ERR("HOME", "blurb malloc failed: %d bytes", static_cast<int>(toRead + 1));
    file.close();
    return;
  }
  const int got = file.read(reinterpret_cast<uint8_t*>(buf), toRead);  // -1 on a read error
  size_t len = got > 0 ? static_cast<size_t>(got) : 0;
  file.close();

  // A partial read can end inside a word or a multi-byte character: cut back
  // to the last space so the wrap only ever sees whole UTF-8 sequences.
  if (len < fileSize) {
    while (len > 0 && buf[len - 1] != ' ' && buf[len - 1] != '\n') --len;
  }
  buf[len] = '\0';
  // Paragraph breaks become spaces: the card is one running excerpt.
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
  }
  if (len > 0) {
    flowText(renderer, flow, kBlurbFontId, buf, INT_MAX);
  }
  free(buf);
  buf = nullptr;
}

void drawHero(const GfxRenderer& renderer, const Layout& l, const RecentBook& book, const int thumbHeight) {
  // drawBitmap only sets black pixels; the snapshot holds a blank card, but a
  // full redraw follows a previous frame's card, so clear it either way --
  // down to heroBottom, since the text can run under the cover.
  renderer.fillRect(l.left, l.heroCover.y, l.right - l.left, l.heroBottom - l.heroCover.y, false);

  const Rect art = drawHeroCover(renderer, book, l.heroCover, thumbHeight);
  if (BookFusionBookIdStore::hasBookId(book.path.c_str())) {
    drawBookFusionBadge(renderer, art);
  }

  // A book with no progress yet (never opened, or reset) is one to start, not
  // continue; the same test drawProgress uses for "Not started".
  const char* heading = book.progressPercent > 0 ? tr(STR_CONTINUE_READING) : tr(STR_START_READING_TITLE);
  renderer.drawText(kLabelFontId, l.heroTextX, l.heroCover.y, heading, true, EpdFontFamily::BOLD);

  // The progress row sits on the card's bottom edge: full width when there is
  // room under the cover for it, otherwise in the column beside the cover.
  const int artBottom = art.y + art.height;
  const int progressBlockH = renderer.getLineHeight(kLabelFontId) + kProgressTextGap + kProgressBarH;
  const int progressTop = l.heroBottom - progressBlockH;
  const bool progressFullWidth = progressTop >= artBottom + kHeroTextGap;
  drawProgress(renderer, progressFullWidth ? l.left : l.heroTextX, progressFullWidth ? l.right - l.left : l.heroTextW,
               l.heroBottom, book.progressPercent);

  // Title, author and blurb share one flow: beside the cover, then full width
  // once a line starts below it, stopping just above the progress row.
  TextFlow flow{l.heroCover.y + renderer.getLineHeight(kLabelFontId) + kHeroLabelGap,
                artBottom + kHeroTextGap,
                l.heroTextX,
                l.heroTextW,
                l.left,
                l.right - l.left,
                progressTop - kHeroLabelGap};

  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  flowText(renderer, flow, kTitleFontId, title, kHeroTitleMaxLines, EpdFontFamily::BOLD);
  if (!book.author.empty()) {
    flow.y += 4;
    flowText(renderer, flow, kAuthorFontId, book.author.c_str(), kHeroAuthorMaxLines);
  }
  flow.y += kHeroBlurbGap;
  drawBlurb(renderer, flow, book);
}

// The shelf slots are recorded by drawShelf, which only runs on frames that
// repaint the shelf -- their positions depend on each cover's aspect, which
// the snapshot frames never re-read.
void recordHeroGeometry(const Layout& l) {
  drawnHero =
      l.showHero ? Rect{l.left, l.heroCover.y, l.right - l.left, l.heroBottom - l.heroCover.y} : Rect{0, 0, 0, 0};
}

}  // namespace

void BookshelfTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                         const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                         bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const ThemeMetrics& metrics = themeMetrics();
  const int bookCount = static_cast<int>(recentBooks.size());
  const Layout layout = computeLayout(renderer, rect, metrics, bookCount);
  recordHeroGeometry(layout);

  if (bookCount == 0) {
    drawEmptyRecents(renderer, Rect{rect.x, rect.y, rect.width, layout.sectionLabelY - rect.y});
    drawPlank(renderer, layout);
    shelfLaidOut = true;
    coverRendered = false;
    coverBufferStored = false;
    focusedIdx = 0;
    drawnSlotCount = 0;
    return;
  }

  if (selectorIndex >= 0 && selectorIndex < layout.slotCount) {
    focusedIdx = selectorIndex;
  }
  focusedIdx = std::clamp(focusedIdx, 0, layout.slotCount - 1);

  // The snapshot covers the whole strip, so a restore that failed leaves it
  // blank: redraw the shelf from SD in that case. The hero card is drawn after
  // the snapshot is taken, so the snapshot never holds a stale card.
  if (!coverRendered || !bufferRestored) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    drawShelf(renderer, layout, recentBooks, metrics.homeCoverHeight);
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }
  shelfLaidOut = true;

  if (layout.showHero) {
    drawHero(renderer, layout, recentBooks[focusedIdx], metrics.homeCoverHeight);
  }
  if (selectorIndex >= 0 && selectorIndex < layout.slotCount) {
    drawShelfSelection(renderer, drawnArtwork[selectorIndex], layout.plankY);
  }
}

int BookshelfTheme::hitTestRecentBookCover(const Rect& rect, const int slotCount, const int lx, const int ly) const {
  if (slotCount <= 0) return -1;
  if (lx < rect.x || lx >= rect.x + rect.width || ly < rect.y || ly >= rect.y + rect.height) return -1;

  const auto contains = [lx, ly](const Rect& r) {
    return r.width > 0 && lx >= r.x && lx < r.x + r.width && ly >= r.y && ly < r.y + r.height;
  };
  if (contains(drawnHero)) {
    return focusedIdx < slotCount ? focusedIdx : -1;
  }
  for (int i = 0; i < drawnSlotCount; ++i) {
    if (contains(drawnSlots[i])) {
      return i < slotCount ? i : -1;
    }
  }
  return -1;
}

void BookshelfTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                    const std::function<std::string(int index)>& buttonLabel,
                                    const std::function<UIIcon(int index)>& rowIcon) const {
  if (!shelfLaidOut) {
    drawnGrid = GridLayout{0, 0, 0, 0, 0};
    LyraTheme::drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, rowIcon);
    return;
  }
  shelfLaidOut = false;
  if (buttonCount <= 0) return;

  drawnGrid = computeGrid(renderer, themeMetrics());
  const int cells = std::min(buttonCount, drawnGrid.cols * drawnGrid.rows);
  for (int cell = 0; cell < cells; ++cell) {
    const int index = cell;
    const std::string label = buttonLabel(index);
    const uint8_t* icon = rowIcon != nullptr ? gridIcon(rowIcon(index)) : nullptr;
    drawGridCell(renderer, gridCell(drawnGrid, cell), label.c_str(), icon, selectedIndex == index);
  }
}

int BookshelfTheme::hitTestButtonMenu(const Rect& rect, const int buttonCount, const int lx, const int ly) const {
  if (drawnGrid.cols <= 0) return LyraTheme::hitTestButtonMenu(rect, buttonCount, lx, ly);
  if (buttonCount <= 0) return -1;

  const auto contains = [lx, ly](const Rect& r) {
    return r.width > 0 && lx >= r.x && lx < r.x + r.width && ly >= r.y && ly < r.y + r.height;
  };
  const int cells = std::min(buttonCount, drawnGrid.cols * drawnGrid.rows);
  for (int cell = 0; cell < cells; ++cell) {
    if (contains(gridCell(drawnGrid, cell))) return cell;
  }
  return -1;
}

int BookshelfTheme::homeNavigate(const int selectorIndex, const int coverCount, const int totalCount,
                                 const NavDirection dir) const {
  // Nothing laid out as a grid (not the home screen), or a sideways press:
  // HomeActivity's linear step is already right.
  if (drawnGrid.cols <= 0 || dir == NavDirection::Left || dir == NavDirection::Right) return -1;

  const int gridStart = coverCount;  // menu item 0 (Library) is the first cell
  const int cells = std::min(totalCount - coverCount, drawnGrid.cols * drawnGrid.rows);
  if (cells <= 0) return -1;

  const auto centerX = [](const Rect& r) { return r.x + r.width / 2; };
  const int books = std::min(coverCount, drawnSlotCount);

  if (selectorIndex < gridStart) {
    if (dir == NavDirection::Up) return selectorIndex;  // top of the page: nowhere to go
    // Down: the top-row cell nearest the book.
    const int x = selectorIndex < books ? centerX(drawnArtwork[selectorIndex]) : 0;
    int best = 0;
    for (int cell = 1; cell < std::min(cells, drawnGrid.cols); ++cell) {
      if (std::abs(centerX(gridCell(drawnGrid, cell)) - x) < std::abs(centerX(gridCell(drawnGrid, best)) - x)) {
        best = cell;
      }
    }
    return gridStart + best;
  }

  const int cell = selectorIndex - gridStart;
  if (dir == NavDirection::Down) {
    const int below = cell + drawnGrid.cols;
    return below < cells ? gridStart + below : selectorIndex;
  }
  if (cell >= drawnGrid.cols) return selectorIndex - drawnGrid.cols;
  if (books <= 0) return selectorIndex;

  // Up from the top row: the book nearest the cell.
  const int x = centerX(gridCell(drawnGrid, cell));
  int best = 0;
  for (int i = 1; i < books; ++i) {
    if (std::abs(centerX(drawnArtwork[i]) - x) < std::abs(centerX(drawnArtwork[best]) - x)) best = i;
  }
  return best;
}
