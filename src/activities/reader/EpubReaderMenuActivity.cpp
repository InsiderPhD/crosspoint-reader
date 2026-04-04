#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const uint32_t timeLeftChapterSeconds,
                                               const uint32_t timeLeftBookSeconds)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      timeLeftChapterSeconds(timeLeftChapterSeconds),
      timeLeftBookSeconds(timeLeftBookSeconds) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes) {
  std::vector<MenuItem> items;
  items.reserve(10);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::READING_SPEED, StrId::STR_READING_SPEED});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::MARK_AS_COMPLETED, StrId::STR_MARK_AS_COMPLETED});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  items.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::READING_SPEED) {
      return;  // display-only row
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: button hints are drawn along a vertical edge, so we
  // reserve a horizontal gutter to prevent overlap with menu content.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: button hints appear near the logical top, so we reserve
  // vertical space to keep the header and list clear.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  // Manual centering so we can respect the content gutter.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Progress summary
  auto formatMins = [](uint32_t secs) -> std::string {
    const uint32_t mins = (secs + 30) / 60;
    char buf[12];
    if (mins < 60) {
      snprintf(buf, sizeof(buf), "~%um", static_cast<unsigned>(mins < 1 ? 1 : mins));
    } else {
      const uint32_t h = mins / 60;
      const uint32_t m = mins % 60;
      if (m == 0) {
        snprintf(buf, sizeof(buf), "~%uh", static_cast<unsigned>(h));
      } else {
        snprintf(buf, sizeof(buf), "~%uh%um", static_cast<unsigned>(h), static_cast<unsigned>(m));
      }
    }
    return std::string(buf);
  };

  std::string progressLine;
  if (totalPages > 0) {
    progressLine = "Ch " + std::to_string(currentPage) + "/" + std::to_string(totalPages);
    if (timeLeftChapterSeconds > 0) {
      progressLine += " " + formatMins(timeLeftChapterSeconds);
    }
    progressLine += "  |  ";
  }
  progressLine += "Book " + std::to_string(bookProgressPercent) + "%";
  if (timeLeftBookSeconds > 0) {
    progressLine += " " + formatMins(timeLeftBookSeconds);
  }

  renderer.drawCenteredText(UI_10_FONT_ID, 45, progressLine.c_str());

  // Menu Items
  const int startY = 75 + contentY;
  constexpr int lineHeight = 30;

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int displayY = startY + (i * lineHeight);
    const bool isSelected = (static_cast<int>(i) == selectedIndex);

    if (isSelected) {
      // Highlight only the content area so we don't paint over hint gutters.
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, I18N.get(menuItems[i].labelId), !isSelected);

    if (menuItems[i].action == MenuAction::ROTATE_SCREEN) {
      // Render current orientation value on the right edge of the content area.
      const char* value = I18N.get(orientationLabels[pendingOrientation]);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }

    if (menuItems[i].action == MenuAction::AUTO_PAGE_TURN) {
      char valueBuf[24];
      if (selectedPageTurnOption == 1 && SETTINGS.readingSpeedSecondsPerPage > 0) {
        snprintf(valueBuf, sizeof(valueBuf), "Auto (~%us)", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage));
      } else {
        snprintf(valueBuf, sizeof(valueBuf), "%s", pageTurnLabels[selectedPageTurnOption]);
      }
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueBuf);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, valueBuf, !isSelected);
    }

    if (menuItems[i].action == MenuAction::READING_SPEED) {
      char valueBuf[16];
      if (SETTINGS.readingSpeedSecondsPerPage == 0) {
        snprintf(valueBuf, sizeof(valueBuf), "--");
      } else if (SETTINGS.readingSpeedSecondsPerPage < 60) {
        snprintf(valueBuf, sizeof(valueBuf), "~%us/page", static_cast<unsigned>(SETTINGS.readingSpeedSecondsPerPage));
      } else {
        const unsigned mins = SETTINGS.readingSpeedSecondsPerPage / 60;
        const unsigned secs = SETTINGS.readingSpeedSecondsPerPage % 60;
        if (secs == 0) {
          snprintf(valueBuf, sizeof(valueBuf), "~%um/page", mins);
        } else {
          snprintf(valueBuf, sizeof(valueBuf), "~%um%us/page", mins, secs);
        }
      }
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueBuf);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, valueBuf, !isSelected);
    }
  }

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
