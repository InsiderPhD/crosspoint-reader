#include "BookshelfTheme.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/bookfusion24.h"
#include "components/icons/cover.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/stats.h"
#include "components/icons/transfer.h"
#include "fontIds.h"

const ThemeMetrics& BookshelfTheme::themeMetrics() const {
#if FREEINK_DEVICE_X4PRO
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
constexpr int kHeroBlurbMaxLines = 10;
constexpr int kBlurbFontId = SMALL_FONT_ID;
// How much of the description sidecar to read for the card. Ten lines of a
// ~200 px column hold well under this, and it keeps the per-selection read
// small next to Epub::MAX_DESCRIPTION_BYTES (4 KB).
constexpr size_t kBlurbReadBytes = 640;
constexpr int kProgressBarH = 6;
constexpr int kProgressTextGap = 6;

// Shelf: books stand bottom-aligned on a plank, packed from the left like a
// real shelf, with the Library stack after the last one.
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

// Library stack: books lying flat, spines facing out. Its width is reserved at
// the end of the shelf whatever the books take, so a full shelf of covers
// never pushes it off the edge.
constexpr int kLibraryW = 72;       // minimum; the stack grows into free shelf to fit its label
constexpr int kLibraryTextPad = 6;  // between the label and each end band
constexpr int kLibraryBottomBookH = 12;
constexpr int kLibraryTopBookPad = 4;  // above and below the label
constexpr int kLibraryTopBookInset = 4;
constexpr int kLibraryBandInset = 6;

// Button grid.
constexpr int kGridCellH = 64;
constexpr int kGridGap = 10;
constexpr int kGridBottomGap = 10;
constexpr int kGridCornerRadius = 10;
constexpr int kGridIconSize = 32;  // must match the bitmaps; drawIcon does not scale
constexpr int kGridIconPad = 16;
constexpr int kGridLabelFontId = UI_12_FONT_ID;
constexpr int kGridMaxCells = 4;

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
// shelf and holds still while the selector is on Library or the grid, so
// leaving the shelf does not snap the card back to the first book.
int focusedIdx = 0;

Rect drawnHero{0, 0, 0, 0};
Rect drawnSlots[kMaxShelfSlots];
int drawnSlotCount = 0;

// Each book's rect as last painted, cover or spine. It depends on the cover's
// aspect, and the selection outline needs it on the frames that reuse the
// snapshot instead of re-reading the thumbnail.
Rect drawnArtwork[kMaxShelfSlots];

// Where the Library stack goes (just after the last book) and the plank under
// it. Set by drawRecentBookCover; drawButtonMenu paints the stack there.
int libraryX = 0;
int libraryPlankY = 0;
int libraryShelfH = 0;
int libraryMaxW = kLibraryW;    // free shelf from libraryX to the right edge
Rect drawnLibrary{0, 0, 0, 0};  // hit area

// True while the selector is on the Library stack: the card then advertises
// the Library instead of a book, and a tap on it opens the Library.
bool heroShowsLibrary = false;

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

// 2x2 in portrait; one row of four in landscape, where a second row would
// leave no height for the card.
GridLayout computeGrid(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);

  GridLayout g{};
  const bool wide = renderer.getScreenWidth() > renderer.getScreenHeight();
  g.cols = wide ? 4 : 2;
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

  // Worst case the shelf holds kMaxFaceOut covers plus the forced spines and
  // the Library stack. Each cover gets an equal share of what those leave,
  // sized for a full shelf even when fewer books exist, so a cover keeps its
  // size as the recents list grows.
  l.slotCount = std::min(bookCount, kMaxShelfSlots);
  const int spinesW = (kMaxShelfSlots - kMaxFaceOut) * (kSpineW + kBookGap);
  const int slotW = (contentW - kLibraryW - kBookGap - spinesW) / kMaxFaceOut;
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
  return l;
}

// ---- covers ----

// Largest size inside the box at the source's own aspect, scaling up as well
// as down (drawBitmapResampled does both), so a small source still fills its
// slot instead of floating tiny in it. Placement is the caller's.
void fitSize(const Bitmap& bitmap, const int boxW, const int boxH, int& wOut, int& hOut) {
  const float sx = static_cast<float>(boxW) / static_cast<float>(bitmap.getWidth());
  const float sy = static_cast<float>(boxH) / static_cast<float>(bitmap.getHeight());
  const float scale = std::min(sx, sy);
  wOut = std::clamp(static_cast<int>(bitmap.getWidth() * scale), 1, boxW);
  hOut = std::clamp(static_cast<int>(bitmap.getHeight() * scale), 1, boxH);
}

void drawPlaceholder(const GfxRenderer& renderer, const Rect& r, const int radius) {
  renderer.fillRoundedRect(r.x, r.y + r.height / 3, r.width, 2 * r.height / 3, radius, /*roundTopLeft=*/false,
                           /*roundTopRight=*/false, /*roundBottomLeft=*/true, /*roundBottomRight=*/true, Color::Black);
  if (r.width > kPlaceholderIconSize + 8 && r.height / 3 > kPlaceholderIconSize / 2) {
    renderer.drawIcon(CoverIcon, r.x + (r.width - kPlaceholderIconSize) / 2, r.y + r.height / 3 + 10,
                      kPlaceholderIconSize, kPlaceholderIconSize);
  }
  renderer.drawRoundedRect(r.x, r.y, r.width, r.height, 1, radius, true);
}

// Draws the book's thumbnail fitted into box: centred horizontally, and either
// bottom-aligned (standing on the shelf) or top-aligned (the hero card).
// Returns the artwork rect actually painted, or the placeholder's rect.
Rect drawCover(const GfxRenderer& renderer, const RecentBook& book, const Rect& box, const bool bottomAlign,
               const int radius, const int thumbHeight) {
  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
    FsFile file;
    if (Storage.openFileForRead("HOME", thumbPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        int w = 0;
        int h = 0;
        fitSize(bitmap, box.width, box.height, w, h);
        const Rect drawn{box.x + (box.width - w) / 2, bottomAlign ? box.y + box.height - h : box.y, w, h};
        // Not drawBitmap: it paints every non-white source pixel black, and a
        // 400 px dithered thumb shrunk to a shelf slot came out solid black.
        renderer.drawBitmapResampled(bitmap, drawn.x, drawn.y, drawn.width, drawn.height);
        renderer.maskRoundedRectOutsideCorners(drawn.x, drawn.y, drawn.width, drawn.height, radius, Color::White);
        renderer.drawRoundedRect(drawn.x, drawn.y, drawn.width, drawn.height, 1, radius, true);
        file.close();
        return drawn;
      }
      file.close();
    }
  }
  // No artwork: a 2:3 placeholder, so a coverless book still reads as a book
  // standing on the shelf rather than filling its whole slot.
  const int w = std::min(box.width, (box.height * 2) / 3);
  const Rect placeholder{box.x + (box.width - w) / 2, box.y, w, box.height};
  drawPlaceholder(renderer, placeholder, radius);
  return placeholder;
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

// Draws the cover standing on the plank with its left edge at x, sized to fit
// maxW x maxH at the artwork's aspect. False (nothing drawn) when there is no
// readable thumbnail, so the caller can stand the book spine-out instead.
bool drawShelfCover(const GfxRenderer& renderer, const RecentBook& book, const int x, const int plankY, const int maxW,
                    const int maxH, const int thumbHeight, Rect& drawnOut) {
  if (book.coverBmpPath.empty()) return false;
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
  FsFile file;
  if (!Storage.openFileForRead("HOME", thumbPath, file)) return false;
  Bitmap bitmap(file);
  bool ok = false;
  if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
    int w = 0;
    int h = 0;
    fitSize(bitmap, maxW, maxH, w, h);
    drawnOut = Rect{x, plankY - h, w, h};
    renderer.drawBitmapResampled(bitmap, drawnOut.x, drawnOut.y, w, h);
    renderer.maskRoundedRectOutsideCorners(drawnOut.x, drawnOut.y, w, h, kShelfCornerRadius, Color::White);
    renderer.drawRoundedRect(drawnOut.x, drawnOut.y, w, h, 1, kShelfCornerRadius, true);
    ok = true;
  }
  file.close();
  return ok;
}

void drawPlank(const GfxRenderer& renderer, const Layout& l) {
  const int x = std::max(0, l.left - kPlankOverhang);
  const int w = std::min(renderer.getScreenWidth(), l.right + kPlankOverhang) - x;
  renderer.fillRect(x, l.plankY, w, kPlankEdgeH, true);
  renderer.fillRectDither(x, l.plankY + kPlankEdgeH, w, kPlankFaceH, Color::LightGray);
  renderer.drawLine(x, l.plankY + kPlankH - 1, x + w - 1, l.plankY + kPlankH - 1, true);
}

// Records where the Library stack stands: at x on this shelf, free to run to
// the shelf's right edge. drawLibraryStack picks the width and the hit rect.
void placeLibrary(const Layout& l, const int x) {
  libraryX = x;
  libraryPlankY = l.plankY;
  libraryShelfH = l.shelfCoverMaxH;
  libraryMaxW = std::max(kLibraryW, l.right - x);
}

void drawShelf(const GfxRenderer& renderer, const Layout& l, const std::vector<RecentBook>& books,
               const int thumbHeight) {
  renderer.drawText(kLabelFontId, l.left, l.sectionLabelY, tr(STR_MENU_RECENT_BOOKS), true, EpdFontFamily::BOLD);

  // Books pack from the left, so a spine takes only its own width and the gap
  // before the Library stack varies like a real shelf. Each book is at most
  // shelfCoverMaxW wide and only the first kMaxFaceOut positions can show a
  // cover, so the stack's reserved width is never overrun.
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
    drawnArtwork[i] = drawn;
    // Hit area: the book's own column, full shelf height down to the plank,
    // widened by half a gap each side so taps between books still land.
    drawnSlots[i] = Rect{drawn.x - kBookGap / 2, l.plankY - l.shelfCoverMaxH, drawn.width + kBookGap, l.shelfCoverMaxH};
    x += drawn.width + kBookGap;
  }
  drawnSlotCount = l.slotCount;
  placeLibrary(l, x + kBookGap);
  drawPlank(renderer, l);
}

// Outline around a book (or the Library stack) plus a caret on the plank
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

// ---- library stack ----

// Two books lying flat on the plank, spines out: a plain one underneath and a
// dark one on top carrying the label. Only as long as the label needs (never
// under kLibraryW), using the free shelf the books left, so the label reads in
// full whenever there is room. Returns the stack's rect.
Rect drawLibraryStack(const GfxRenderer& renderer, const char* label) {
  const int x = libraryX;
  const int plankY = libraryPlankY;
  if (label == nullptr) label = "";

  const int labelW = renderer.getTextWidth(kSpineFontId, label, EpdFontFamily::BOLD);
  const int wantW = labelW + 2 * (kLibraryTopBookInset + kLibraryBandInset + kLibraryTextPad);
  const int stackW = std::clamp(wantW, kLibraryW, libraryMaxW);
  drawnLibrary = Rect{x - kBookGap / 2, plankY - libraryShelfH, stackW + kBookGap, libraryShelfH};

  const Rect under{x, plankY - kLibraryBottomBookH, stackW, kLibraryBottomBookH};
  renderer.fillRect(under.x, under.y, under.width, under.height, false);
  renderer.drawRoundedRect(under.x, under.y, under.width, under.height, 1, 2, true);
  renderer.drawLine(under.x + kLibraryBandInset, under.y + 2, under.x + kLibraryBandInset, under.y + under.height - 3,
                    true);
  renderer.drawLine(under.x + under.width - 1 - kLibraryBandInset, under.y + 2,
                    under.x + under.width - 1 - kLibraryBandInset, under.y + under.height - 3, true);

  const int topH = renderer.getLineHeight(kSpineFontId) + 2 * kLibraryTopBookPad;
  const Rect top{x + kLibraryTopBookInset, under.y - topH, stackW - 2 * kLibraryTopBookInset, topH};
  renderer.fillRoundedRect(top.x, top.y, top.width, top.height, 2, Color::Black);
  renderer.drawLine(top.x + kLibraryBandInset, top.y + 2, top.x + kLibraryBandInset, top.y + top.height - 3, false);
  renderer.drawLine(top.x + top.width - 1 - kLibraryBandInset, top.y + 2, top.x + top.width - 1 - kLibraryBandInset,
                    top.y + top.height - 3, false);

  const int textMaxW = top.width - 2 * (kLibraryBandInset + kLibraryTextPad);
  if (textMaxW > 0) {
    const std::string visible = renderer.truncatedText(kSpineFontId, label, textMaxW, EpdFontFamily::BOLD);
    const int textW = renderer.getTextWidth(kSpineFontId, visible.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(kSpineFontId, top.x + (top.width - textW) / 2, top.y + kLibraryTopBookPad, visible.c_str(), false,
                      EpdFontFamily::BOLD);
  }
  return Rect{x, top.y, stackW, plankY - top.y};
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

void drawProgress(const GfxRenderer& renderer, const Layout& l, const int bottomY, const int progressPercent) {
  char buf[40];
  if (progressPercent < 0 || progressPercent == 0) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BOOK_INFO_NOT_STARTED));
  } else if (progressPercent >= 100) {
    snprintf(buf, sizeof(buf), "%s", tr(STR_BOOK_FINISHED));
  } else {
    snprintf(buf, sizeof(buf), "%d%%", progressPercent);
  }
  const int textY = bottomY - renderer.getLineHeight(kLabelFontId);
  renderer.drawText(kLabelFontId, l.heroTextX, textY, buf, true);

  if (progressPercent <= 0) return;
  const int barY = textY - kProgressTextGap - kProgressBarH;
  const int pct = std::min(100, progressPercent);
  renderer.drawRoundedRect(l.heroTextX, barY, l.heroTextW, kProgressBarH, 1, kProgressBarH / 2, true);
  const int fillW = (l.heroTextW * pct) / 100;
  if (fillW >= kProgressBarH) {
    renderer.fillRoundedRect(l.heroTextX, barY, fillW, kProgressBarH, kProgressBarH / 2, Color::Black);
  }
}

// As much of the book's description (the plain-text sidecar the metadata pass
// writes) as fits in maxLines from y; wrappedText ellipsises the last line.
// Only the first kBlurbReadBytes are read: the whole 4 KB would be wasted on a
// card that shows a few hundred characters. Heap, not stack, because the
// buffer is past the 256 B stack budget; it is freed before returning.
void drawBlurb(const GfxRenderer& renderer, const Layout& l, const RecentBook& book, const int y, const int maxLines) {
  if (maxLines <= 0 || !FsHelpers::hasEpubExtension(book.path)) return;
  // Constructing an Epub only derives its cache path; nothing is loaded.
  const Epub epub(book.path, "/.crosspoint");
  FsFile file;
  if (!Storage.openFileForRead("HOME", epub.getDescriptionPath(), file)) return;

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
  // to the last space so wrappedText only ever sees whole UTF-8 sequences.
  if (len < fileSize) {
    while (len > 0 && buf[len - 1] != ' ' && buf[len - 1] != '\n') --len;
  }
  buf[len] = '\0';
  // Paragraph breaks become spaces: the card is one running excerpt.
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') buf[i] = ' ';
  }

  if (len > 0) {
    const int lineH = renderer.getLineHeight(kBlurbFontId);
    int lineY = y;
    for (const auto& line : renderer.wrappedText(kBlurbFontId, buf, l.heroTextW, maxLines)) {
      renderer.drawText(kBlurbFontId, l.heroTextX, lineY, line.c_str(), true);
      lineY += lineH;
    }
  }
  free(buf);
  buf = nullptr;
}

void drawHero(const GfxRenderer& renderer, const Layout& l, const RecentBook& book, const int thumbHeight) {
  // drawBitmap only sets black pixels; the snapshot holds a blank card, but a
  // full redraw follows a previous frame's card, so clear it either way.
  renderer.fillRect(l.left, l.heroCover.y, l.right - l.left, l.heroCover.height, false);

  const Rect art = drawCover(renderer, book, l.heroCover, /*bottomAlign=*/false, kHeroCornerRadius, thumbHeight);
  if (BookFusionBookIdStore::hasBookId(book.path.c_str())) {
    drawBookFusionBadge(renderer, art);
  }

  int y = l.heroCover.y;
  // A book with no progress yet (never opened, or reset) is one to start, not
  // continue; the same test drawProgress uses for "Not started".
  const char* heading = book.progressPercent > 0 ? tr(STR_CONTINUE_READING) : tr(STR_START_READING_TITLE);
  renderer.drawText(kLabelFontId, l.heroTextX, y, heading, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kLabelFontId) + kHeroLabelGap;

  // Reserve the progress block at the card's bottom, then give the title and
  // author only the lines that fit above it (landscape shrinks the card).
  const int progressBlockH = renderer.getLineHeight(kLabelFontId) + kProgressTextGap + kProgressBarH;
  const int textBottom = art.y + art.height - progressBlockH - kHeroLabelGap;
  const int titleLineH = renderer.getLineHeight(kTitleFontId);
  const int authorLineH = renderer.getLineHeight(kAuthorFontId);

  const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
  const int titleLines = std::clamp((textBottom - y) / titleLineH, 0, kHeroTitleMaxLines);
  if (titleLines > 0) {
    for (const auto& line : renderer.wrappedText(kTitleFontId, title, l.heroTextW, titleLines, EpdFontFamily::BOLD)) {
      renderer.drawText(kTitleFontId, l.heroTextX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += titleLineH;
    }
  }

  const int authorLines = std::clamp((textBottom - y - 4) / authorLineH, 0, kHeroAuthorMaxLines);
  if (!book.author.empty() && authorLines > 0) {
    y += 4;
    for (const auto& line : renderer.wrappedText(kAuthorFontId, book.author.c_str(), l.heroTextW, authorLines)) {
      renderer.drawText(kAuthorFontId, l.heroTextX, y, line.c_str(), true);
      y += authorLineH;
    }
  }

  const int blurbLines =
      std::min(kHeroBlurbMaxLines, (textBottom - y - kHeroBlurbGap) / renderer.getLineHeight(kBlurbFontId));
  drawBlurb(renderer, l, book, y + kHeroBlurbGap, blurbLines);

  drawProgress(renderer, l, art.y + art.height, book.progressPercent);
}

// The card while the Library stack is selected. The stack's own label is only
// a few letters wide, so this is where the Library is actually named: a
// light box with the library icon in place of a cover, and "View Library".
void drawLibraryHero(const GfxRenderer& renderer, const Layout& l) {
  renderer.fillRect(l.left, l.heroCover.y, l.right - l.left, l.heroCover.height, false);

  const Rect& box = l.heroCover;
  renderer.fillRoundedRect(box.x, box.y, box.width, box.height, kHeroCornerRadius, Color::LightGray);
  renderer.drawRoundedRect(box.x, box.y, box.width, box.height, 1, kHeroCornerRadius, true);
  renderer.drawIcon(LibraryIcon, box.x + (box.width - kGridIconSize) / 2, box.y + (box.height - kGridIconSize) / 2,
                    kGridIconSize, kGridIconSize);

  int y = box.y;
  renderer.drawText(kLabelFontId, l.heroTextX, y, tr(STR_LIBRARY), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(kLabelFontId) + kHeroLabelGap;
  for (const auto& line :
       renderer.wrappedText(kTitleFontId, tr(STR_VIEW_LIBRARY), l.heroTextW, kHeroTitleMaxLines, EpdFontFamily::BOLD)) {
    renderer.drawText(kTitleFontId, l.heroTextX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(kTitleFontId);
  }
  y += 4;
  for (const auto& line :
       renderer.wrappedText(kAuthorFontId, tr(STR_VIEW_LIBRARY_HINT), l.heroTextW, kHeroAuthorMaxLines)) {
    renderer.drawText(kAuthorFontId, l.heroTextX, y, line.c_str(), true);
    y += renderer.getLineHeight(kAuthorFontId);
  }
}

// The shelf slots are recorded by drawShelf, which only runs on frames that
// repaint the shelf -- their positions depend on each cover's aspect, which
// the snapshot frames never re-read.
void recordHeroGeometry(const Layout& l) {
  drawnHero = l.showHero ? Rect{l.left, l.heroCover.y, l.right - l.left, l.heroCover.height} : Rect{0, 0, 0, 0};
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
    placeLibrary(layout, layout.left + kBookGap / 2);
    shelfLaidOut = true;
    coverRendered = false;
    coverBufferStored = false;
    focusedIdx = 0;
    drawnSlotCount = 0;
    heroShowsLibrary = false;
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

  // HomeActivity numbers the menu straight after the books, and Library is
  // its first item, so the selector sits on the stack at index bookCount.
  heroShowsLibrary = layout.showHero && selectorIndex == bookCount;
  if (heroShowsLibrary) {
    drawLibraryHero(renderer, layout);
  } else if (layout.showHero) {
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
    // Showing the Library, the card belongs to the menu: hitTestButtonMenu
    // resolves it.
    if (heroShowsLibrary) return -1;
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

  // Item 0 is Library: it lives on the shelf.
  const std::string libraryLabel = buttonLabel(0);
  const Rect stack = drawLibraryStack(renderer, libraryLabel.c_str());
  if (selectedIndex == 0) {
    drawShelfSelection(renderer, stack, libraryPlankY);
  }

  drawnGrid = computeGrid(renderer, themeMetrics());
  const int cells = std::min(buttonCount - 1, drawnGrid.cols * drawnGrid.rows);
  for (int cell = 0; cell < cells; ++cell) {
    const int index = cell + 1;
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
  if (contains(drawnLibrary)) return 0;
  if (heroShowsLibrary && contains(drawnHero)) return 0;
  const int cells = std::min(buttonCount - 1, drawnGrid.cols * drawnGrid.rows);
  for (int cell = 0; cell < cells; ++cell) {
    if (contains(gridCell(drawnGrid, cell))) return cell + 1;
  }
  return -1;
}

int BookshelfTheme::homeNavigate(const int selectorIndex, const int coverCount, const int totalCount,
                                 const NavDirection dir) const {
  // Nothing laid out as a grid (not the home screen), or a sideways press:
  // HomeActivity's linear step is already right.
  if (drawnGrid.cols <= 0 || dir == NavDirection::Left || dir == NavDirection::Right) return -1;

  const int libraryIdx = coverCount;  // Library is menu item 0, straight after the books
  const int cells = std::min(totalCount - coverCount - 1, drawnGrid.cols * drawnGrid.rows);
  if (cells <= 0) return -1;

  const auto centerX = [](const Rect& r) { return r.x + r.width / 2; };
  const auto shelfItemX = [&](const int idx) {
    if (idx < coverCount && idx < drawnSlotCount) return centerX(drawnArtwork[idx]);
    return centerX(drawnLibrary);
  };

  if (selectorIndex <= libraryIdx) {
    if (dir == NavDirection::Up) return selectorIndex;  // top of the page: nowhere to go
    // Down: the top-row cell nearest the shelf item.
    const int x = shelfItemX(selectorIndex);
    int best = 0;
    for (int cell = 1; cell < std::min(cells, drawnGrid.cols); ++cell) {
      if (std::abs(centerX(gridCell(drawnGrid, cell)) - x) < std::abs(centerX(gridCell(drawnGrid, best)) - x)) {
        best = cell;
      }
    }
    return libraryIdx + 1 + best;
  }

  const int cell = selectorIndex - libraryIdx - 1;
  if (dir == NavDirection::Down) {
    const int below = cell + drawnGrid.cols;
    return below < cells ? libraryIdx + 1 + below : selectorIndex;
  }
  if (cell >= drawnGrid.cols) return selectorIndex - drawnGrid.cols;

  // Up from the top row: the shelf item nearest the cell, Library included.
  const int x = centerX(gridCell(drawnGrid, cell));
  int best = libraryIdx;
  for (int i = 0; i < coverCount; ++i) {
    if (std::abs(shelfItemX(i) - x) < std::abs(shelfItemX(best) - x)) best = i;
  }
  return best;
}
