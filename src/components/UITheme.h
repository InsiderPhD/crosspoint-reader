#pragma once

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

  // Book context menu — shared across HomeActivity, FileBrowserActivity,
  // and RecentBooksActivity.
  static constexpr int BOOK_OPTIONS_COUNT = 7;
  static constexpr int BOOK_OPT_MARK_READ = 0;
  static constexpr int BOOK_OPT_RESET_PROGRESS = 1;
  static constexpr int BOOK_OPT_SHELVE = 2;
  static constexpr int BOOK_OPT_DELETE = 3;
  static constexpr int BOOK_OPT_REINDEX = 4;
  static constexpr int BOOK_OPT_BOOK_INFO = 5;
  static constexpr int BOOK_OPT_DELETE_CLIPPINGS = 6;

  // Fills ids[] with the BOOK_OPT_* values currently visible (honoring Dev Mode —
  // Delete Book Cache is hidden when Dev Mode is off; Delete Clippings appears only when
  // the book has a clippings file) and returns the count. Used by both
  // drawBookOptionsPopup (labels) and the host activities (index -> option id).
  static int getVisibleBookOptions(int* ids, int maxIds, bool includeDeleteClippings);

  // Layout of the popup just drawn, for Full Touch tap hit-testing: option
  // row i spans y in [optionsTopY + i*optionRowH, optionsTopY + (i+1)*optionRowH).
  struct BookOptionsPopupLayout {
    Rect popup;
    int optionsTopY;
    int optionRowH;
  };
  static BookOptionsPopupLayout drawBookOptionsPopup(GfxRenderer& renderer, const char* title, const char* author,
                                                     const char* folderPath, int progressPercent,
                                                     int selectedOptionIndex, bool includeDeleteClippings);

  static void drawSyncProgressPopup(GfxRenderer& renderer, const char* title, const char* statusMessage);

  // Where the button hints sit, expressed as a per-edge reserve in the CURRENT
  // logical frame. Screens subtract these before laying out, so content never
  // runs under the hints -- and, just as important, never reserves an edge the
  // hints are not on, which shows up as an empty margin down one side.
  //
  // Front-button boards (X3/X4): BaseTheme::drawButtonHints() forces portrait
  // and paints along the panel's physical bottom so each label lands under its
  // physical button. In a landscape logical frame that strip is a vertical
  // band -- the left edge in Landscape CW, the right edge in CCW -- and in
  // inverted portrait it is the top (see rotateCoordinates() in
  // GfxRenderer.cpp).
  //
  // X4 Pro: there are no front buttons to label. ActionBar::draw() paints tap
  // targets in the logical frame, along the bottom in EVERY orientation, so the
  // only reserve is the bar's height (already 0 in gesture mode).
  //
  // sideGutter / invertedTopGutter stay per-screen: the existing screens use
  // different widths (30, 40, sideButtonHintsWidth) and this must not silently
  // relayout them.
  struct HintReserve {
    int left;
    int right;
    int top;
    int bottom;
  };
  static HintReserve getHintReserve(const GfxRenderer& renderer, int sideGutter, int invertedTopGutter);

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
