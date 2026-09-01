#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 40,
                                 .listWithSubtitleRowHeight = 60,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .pageIndicatorHeight = 30,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
#if FREEINK_DEVICE_X4PRO
                                 // X4 Pro has no front buttons, so this strip holds the tappable
                                 // action bar instead of four button labels (see ActionBar.h).
                                 // Gesture mode reclaims it via noActionBarValues below.
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
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 6};

#if FREEINK_DEVICE_X4PRO
// Gesture mode (Full Touch off) reclaims the action-bar strip. See the same
// pair in BaseMetrics; LyraTheme::themeMetrics() is out of line for the same
// reason (the choice needs SETTINGS).
constexpr ThemeMetrics noActionBarValues = [] {
  ThemeMetrics v = values;
  v.buttonHintsHeight = 0;
  return v;
}();
#endif
}  // namespace LyraMetrics

class LyraTheme : public BaseTheme {
 public:
  // Lyra draws lists/tabs/menus against its own metrics table; overriding this
  // keeps the shared geometry helpers (listGeometry, hitTest*) in lockstep
  // with the Lyra draw code. Lyra3Covers/LyraLibrary inherit it — their metrics
  // differ only in home-tile fields no list geometry reads.
  const ThemeMetrics& themeMetrics() const override;

  // Component drawing methods
  //   void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) override;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  const char* powerButtonHintLabel = nullptr) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const char* pageIndicatorOverride = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                       bool allSlots) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawPowerButtonHint(GfxRenderer& renderer, const char* label) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const override;
  bool showsFileIcons() const override { return true; }

 protected:
  // Lyra tab cells are text + fixed padding (no bold-when-selected, no badge).
  int tabCellWidth(const GfxRenderer& renderer, const TabInfo& tab) const override;
  // Lyra's drawButtonMenu starts tiles flush with the rect top (Classic leads
  // with verticalSpacing).
  int buttonMenuTopOffset() const override { return 0; }
};
