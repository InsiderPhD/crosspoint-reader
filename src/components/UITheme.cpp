#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "fontIds.h"

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraLibraryTheme.h"
#include "components/themes/lyra/LyraTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_LIBRARY:
      LOG_DBG("UI", "Using Lyra Library theme");
      currentTheme = std::make_unique<LyraLibraryTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;  // Identical layout metrics.
      break;
    default:
      LOG_DBG("UI", "Unknown theme %d, falling back to Lyra", static_cast<int>(type));
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

void UITheme::drawBookOptionsPopup(GfxRenderer& renderer, const char* title, const char* author,
                                   const char* folderPath, int progressPercent, int selectedOptionIndex) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int POPUP_W = 420;
  constexpr int BORDER = 2;
  constexpr int H_PAD = 14;
  constexpr int INFO_H = 28;
  constexpr int INFO_COUNT = 3;
  constexpr int OPTION_H = 34;
  constexpr int LINE_V_PAD = 6;

  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title, POPUP_W - H_PAD * 2, 2);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int titleBlockH = LINE_V_PAD + static_cast<int>(titleLines.size()) * lineH + LINE_V_PAD;

  const int popupH = titleBlockH + INFO_COUNT * INFO_H + BOOK_OPTIONS_COUNT * OPTION_H + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - popupH) / 2;

  renderer.fillRect(px, py, POPUP_W, popupH, false);
  renderer.drawRect(px, py, POPUP_W, popupH, BORDER, true);

  for (int i = 0; i < static_cast<int>(titleLines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + LINE_V_PAD + i * lineH, titleLines[i].c_str(), true);
  }
  renderer.drawLine(px + BORDER, py + titleBlockH, px + POPUP_W - BORDER - 1, py + titleBlockH);

  char infoBuf[80];
  snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_AUTHOR), (author && author[0]) ? author : "--");
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + titleBlockH + (INFO_H - lineH) / 2, infoBuf);

  const std::string truncFolder = renderer.truncatedText(UI_10_FONT_ID, folderPath, POPUP_W - H_PAD * 2);
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + titleBlockH + INFO_H + (INFO_H - lineH) / 2, truncFolder.c_str());

  if (progressPercent < 0) {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %s", tr(STR_BOOK_INFO_PROGRESS), tr(STR_BOOK_INFO_NOT_STARTED));
  } else {
    snprintf(infoBuf, sizeof(infoBuf), "%s: %d%%", tr(STR_BOOK_INFO_PROGRESS), progressPercent);
  }
  renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + titleBlockH + INFO_H * 2 + (INFO_H - lineH) / 2, infoBuf);

  renderer.drawLine(px + BORDER, py + titleBlockH + INFO_COUNT * INFO_H, px + POPUP_W - BORDER - 1,
                    py + titleBlockH + INFO_COUNT * INFO_H);

  const char* options[BOOK_OPTIONS_COUNT] = {tr(STR_MARK_AS_READ), tr(STR_RESET_PROGRESS), tr(STR_REMOVE_FROM_RECENTS),
                                             tr(STR_DELETE_FROM_DEVICE), tr(STR_DELETE_CACHE)};
  for (int i = 0; i < BOOK_OPTIONS_COUNT; i++) {
    const int optY = py + titleBlockH + INFO_COUNT * INFO_H + i * OPTION_H;
    if (i == selectedOptionIndex) {
      renderer.fillRect(px + BORDER, optY, POPUP_W - BORDER * 2, OPTION_H, true);
    }
    renderer.drawText(UI_10_FONT_ID, px + H_PAD * 2, optY + (OPTION_H - lineH) / 2, options[i],
                      i != selectedOptionIndex);
  }
}

void UITheme::drawSyncProgressPopup(GfxRenderer& renderer, const char* title, const char* statusMessage) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int POPUP_W = 320;
  constexpr int BORDER = 2;
  constexpr int H_PAD = 16;
  constexpr int LINE_V_PAD = 12;

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Calculate height based on content
  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, title, POPUP_W - H_PAD * 2, 2);
  const int titleBlockH = LINE_V_PAD + static_cast<int>(titleLines.size()) * lineH + LINE_V_PAD;

  const auto statusLines = renderer.wrappedText(UI_10_FONT_ID, statusMessage, POPUP_W - H_PAD * 2, 3);
  const int statusBlockH = LINE_V_PAD + static_cast<int>(statusLines.size()) * lineH + LINE_V_PAD;

  const int popupH = titleBlockH + statusBlockH + H_PAD;
  const int px = (pageWidth - POPUP_W) / 2;
  const int py = (pageHeight - popupH) / 2;

  // Draw popup background and border
  renderer.fillRect(px, py, POPUP_W, popupH, false);
  renderer.drawRect(px, py, POPUP_W, popupH, BORDER, true);

  // Draw title
  for (int i = 0; i < static_cast<int>(titleLines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + LINE_V_PAD + i * lineH, titleLines[i].c_str(), true);
  }

  // Draw separator line
  renderer.drawLine(px + BORDER, py + titleBlockH, px + POPUP_W - BORDER - 1, py + titleBlockH);

  // Draw status message
  for (int i = 0; i < static_cast<int>(statusLines.size()); i++) {
    renderer.drawText(UI_10_FONT_ID, px + H_PAD, py + titleBlockH + LINE_V_PAD + i * lineH,
                     statusLines[i].c_str(), true);
  }
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}
