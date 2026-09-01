#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct TabInfo {
  const char* label;
  bool selected;
  bool hasBadge = false;  // Draws a small dot after the label (e.g. "needs attention")
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int tabSpacing;
  int tabBarHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;
  // Height of the strip every scrollable surface gives up at the bottom of its
  // content rect for the "n / m" page count (see BaseTheme::pageIndicatorRect).
  int pageIndicatorHeight;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;
  // Extra gap reserved above the status bar (lifts body content up); the bar text
  // itself stays anchored to the bottom (see BaseTheme::drawStatusBar).
  int statusBarContentGap;

  int keyboardKeyWidth;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  int keyboardBottomKeyHeight;
  int keyboardBottomKeySpacing;
  bool keyboardBottomAligned;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;
  int keyboardKeyCornerRadius;
};

enum UIIcon {
  Folder,
  Text,
  Image,
  Book,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  BookFusion,
  Star,
  Arrow,
  Check,
  Files,
  None,  // Render nothing — the row reserves icon space but draws blank.
};

enum class KeyboardKeyType { Normal, Shift, Mode, Space, Del, Ok, Disabled };

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 65,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .pageIndicatorHeight = 30,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
#if FREEINK_DEVICE_X4PRO
                                 // X4 Pro has no front buttons, so this strip holds the tappable
                                 // action bar instead of four button labels (see ActionBar.h).
                                 // A touch target wants more than the 40px a label needed.
                                 // Gesture mode reclaims the strip via noActionBarValues below.
                                 .buttonHintsHeight = 44,
#else
                                 .buttonHintsHeight = 40,
#endif
                                 .sideButtonHintsWidth = 40,  // match buttonHintsHeight so side/power boxes
                                                              // are the same thickness as the front hint bar
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .statusBarContentGap = 3,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 0};

#if FREEINK_DEVICE_X4PRO
// Gesture mode (Full Touch off): nothing is drawn in the hint strip, so screens
// reclaim it. BaseTheme::themeMetrics() picks between the two at runtime, which
// is why it is defined out of line -- the choice needs SETTINGS.
constexpr ThemeMetrics noActionBarValues = [] {
  ThemeMetrics v = values;
  v.buttonHintsHeight = 0;
  return v;
}();
#endif
}  // namespace BaseMetrics

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // The metrics table this theme draws with. Every geometry consumer below
  // (listGeometry, hitTestList, hitTestTabBar, hitTestButtonMenu) reads this,
  // so a theme that overrides drawList/drawTabBar against its own table must
  // override this too — that single override keeps paint and hit-test in
  // lockstep.
  virtual const ThemeMetrics& themeMetrics() const;

  // Row geometry of a drawList call. Single source for drawList AND
  // hitTestList so the two can never drift.
  struct ListGeometry {
    int rowHeight;
    int pageItems;      // rows per page, >= 1
    int pageStart;      // first visible item index
    int contentHeight;  // rect.height minus the page-indicator strip
  };
  ListGeometry listGeometry(const Rect& rect, int selectedIndex, bool hasSubtitle) const;

  // --- The page counter -------------------------------------------------
  //
  // A scroll bar is easy to miss, so every surface that pages also says so in
  // words, and always in the SAME place: the bottom strip of its content rect,
  // centred, "3 / 7". The strip is reserved unconditionally -- a list that
  // grows past one page must not shove its own rows up -- and painted only
  // when there is more than one page.
  //
  // drawList does this for itself, so a list screen gets the counter for free
  // and its rows already sit clear of the strip (listGeometry takes it out).
  // Screens that page their own content (the cover grid, the reader menu, the
  // stats pages) call drawPageIndicator with the SAME rect they laid that
  // content out in, and must reserve pageIndicatorRect(rect).height themselves.
  //
  // Returns a zero-height rect when `rect` is too short to give up the space;
  // nothing is then reserved and nothing is drawn.
  Rect pageIndicatorRect(const Rect& rect) const;

  // Height a list rect needs to show `rowCount` rows AND carry the strip. The
  // fixed forms (the date spinners) size themselves with this: they have
  // exactly as many rows as fields, so an unaccounted-for strip would cost
  // them a field rather than a page.
  int listRectHeightForRows(int rowCount, bool hasSubtitle) const;

  // Height left for content once the strip is taken out of `rect`.
  int contentHeightWithoutIndicator(const Rect& rect) const;

  // No-op when totalPages <= 1 (there is nothing to tell the user).
  void drawPageIndicator(const GfxRenderer& renderer, const Rect& rect, int currentPage, int totalPages) const;

  // Same strip, caller's wording -- for screens whose pages are not this
  // rect's rows (BookFusion pages over the network, and needs a "7+" when the
  // server did not say how many there are). Empty/null text draws nothing.
  void drawPageIndicatorText(const GfxRenderer& renderer, const Rect& rect, const char* text) const;

  // Item index under a logical-frame point (Full Touch tap dispatch), or -1 on
  // a miss. rect/selectedIndex/hasSubtitle MUST match the concurrent drawList
  // call — they determine the visible page and the row height.
  int hitTestList(const Rect& rect, int itemCount, int selectedIndex, bool hasSubtitle, int lx, int ly) const;

  // Tab index under the point for the ribbon drawTabBar painted, or -1. Takes
  // the live TabInfo vector because cell widths depend on selection state
  // (Classic measures the selected tab bold) and the scroll offset tracks it.
  int hitTestTabBar(const GfxRenderer& renderer, const Rect& rect, const std::vector<TabInfo>& tabs, int lx,
                    int ly) const;

  // Tile index under the point for the menu drawButtonMenu painted, or -1.
  int hitTestButtonMenu(const Rect& rect, int buttonCount, int lx, int ly) const;

  // Cover slot under the point within the strip drawRecentBookCover painted, or
  // -1. Single-cover themes (homeRecentBooksCount == 1) treat the whole strip as
  // slot 0; multi-cover themes split it into that many equal columns inset by
  // contentSidePadding, matching Lyra3CoversTheme's tile layout.
  int hitTestRecentBookCover(const Rect& rect, int slotCount, int lx, int ly) const;

  // Component drawing methods
  virtual void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  virtual void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                               bool showPercentage = true) const;  // Left aligned (reader mode)
  virtual void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                                bool showPercentage = true) const;  // Right aligned (UI headers)
  // allSlots is an X4 Pro concern only. There the labels become the Full Touch
  // action bar's tap targets, and only Back/Confirm get one by default -- see
  // ActionBar.h. A screen whose Left/Right are ACTIONS rather than directions
  // passes true to have all four drawn. Boards with real front buttons always
  // draw all four labels and ignore it.
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4, bool allSlots = false) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  // Side-mounted Power-button hint: a bordered box styled like the front-button hints but
  // rotated 90° (sideways), drawn on the left edge. Used by list screens where a Power
  // short-press opens the sort menu. No-op for empty labels.
  virtual void drawPowerButtonHint(GfxRenderer& renderer, const char* label) const;
  // pageIndicatorOverride: what to print in the page-counter strip instead of
  // this list's own "page of pages" (see drawPageIndicatorText). nullptr = the
  // computed count, "" = draw nothing there.
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr,
                        const char* pageIndicatorOverride = nullptr) const;
  // powerButtonHintLabel: when non-null, the caller also draws the side Power-button hint with this
  // label (e.g. "Sort"). On the X3 that hint box sits in the top-right corner where the battery
  // renders, so the theme shifts the battery clear of the box (sized to this label). Null = no hint.
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                          const char* powerButtonHintLabel = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                   bool& bufferRestored, std::function<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  // Draws the reader status bar. Each element (battery, book/chapter title,
  // book %, chapter page count, book/chapter time-left, clock, bookmark) carries
  // its own Hide/Left/Middle/Right position in settings and is laid out into the
  // matching cluster here. centerOverride, when non-empty, replaces the titles
  // with a single centred string (used for the auto page-turn banner).
  virtual void drawStatusBar(GfxRenderer& renderer, float bookProgress, int currentPage, int pageCount,
                             const std::string& bookTitle, const std::string& chapterTitle,
                             uint32_t chapterTimeLeftSeconds = 0, uint32_t bookTimeLeftSeconds = 0,
                             bool isPageBookmarked = false, int paddingBottom = 0, int textYOffset = 0,
                             const std::string& centerOverride = std::string()) const;
  virtual void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                               const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                               bool inactiveSelection = false) const;
  virtual bool showsFileIcons() const { return false; }

  // Returns the home-screen slot index that should launch the library view
  // when activated, or -1 if this theme does not surface a library tile.
  // Themes opt in by overriding (e.g. LyraLibraryTheme returns 2 — third tile).
  virtual int getLibrarySlotIndex() const { return -1; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);

 protected:
  // Tab badge dot (Classic theme): drawn after the label, so part of the cell width.
  static constexpr int TAB_BADGE_SIZE = 5;  // diameter of the filled square dot
  static constexpr int TAB_BADGE_GAP = 3;   // gap between text right edge and dot left edge

  // Drawn width of one tab cell — the theme-specific half of the tab-bar
  // geometry (font, bold-when-selected, padding, badge). Used by drawTabBar's
  // advance, computeTabScrollOffset, and hitTestTabBar; override alongside
  // drawTabBar or the ribbon and the hit-test disagree.
  virtual int tabCellWidth(const GfxRenderer& renderer, const TabInfo& tab) const;

  // Horizontal scroll offset of the tab ribbon (the PEEK-clamped pass-1 math,
  // identical in both themes). Shared by drawTabBar and hitTestTabBar.
  int computeTabScrollOffset(const GfxRenderer& renderer, const Rect& rect, const std::vector<TabInfo>& tabs) const;

  // Vertical offset of the first drawButtonMenu tile inside its rect: Classic
  // leads with verticalSpacing, Lyra starts flush. Keep in lockstep with the
  // theme's drawButtonMenu.
  virtual int buttonMenuTopOffset() const { return themeMetrics().verticalSpacing; }
};
