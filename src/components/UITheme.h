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
  static constexpr int BOOK_OPTIONS_COUNT = 6;
  static constexpr int BOOK_OPT_MARK_READ = 0;
  static constexpr int BOOK_OPT_RESET_PROGRESS = 1;
  static constexpr int BOOK_OPT_SHELVE = 2;
  static constexpr int BOOK_OPT_DELETE = 3;
  static constexpr int BOOK_OPT_REINDEX = 4;
  static constexpr int BOOK_OPT_BOOK_INFO = 5;

  // Fills ids[] with the BOOK_OPT_* values currently visible (honoring Dev Mode —
  // Delete Book Cache is hidden when Dev Mode is off) and returns the count. Used by
  // both drawBookOptionsPopup (labels) and the host activities (index -> option id).
  static int getVisibleBookOptions(int* ids, int maxIds);

  static void drawBookOptionsPopup(GfxRenderer& renderer, const char* title, const char* author, const char* folderPath,
                                   int progressPercent, int selectedOptionIndex);

  static void drawSyncProgressPopup(GfxRenderer& renderer, const char* title, const char* statusMessage);

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
