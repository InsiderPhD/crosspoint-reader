#include "EpubReaderMenuActivity.h"

#include <BluetoothHIDManager.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"
#include "util/TimeUtils.h"
#include "util/TouchListNav.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const ProgressAutoSync::Provider syncProvider,
                                               const uint32_t timeLeftChapterSeconds,
                                               const uint32_t timeLeftBookSeconds)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, syncProvider)),
      title(title),
      pendingOrientation(currentOrientation),
      pendingButtonHints(SETTINGS.showButtonHints),
      pendingAutosyncMode(SETTINGS.autosyncMode),
      pendingFrontlightBrightness(SETTINGS.frontlightBrightness),
      pendingFrontlightWarmth(SETTINGS.frontlightWarmth),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      timeLeftChapterSeconds(timeLeftChapterSeconds),
      timeLeftBookSeconds(timeLeftBookSeconds) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(
    bool hasFootnotes, ProgressAutoSync::Provider syncProvider) {
  std::vector<MenuItem> items;
  items.reserve(23);  // 10 fixed + footnotes + frontlight + bookmarks/clippings/autosync+sync + 2 dev-mode items
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::BUTTON_HINTS, StrId::STR_SHOW_BUTTON_HINTS});
  items.push_back({MenuAction::DARK_MODE, StrId::STR_READER_DARK_MODE});
#if FREEINK_CAP_FRONTLIGHT
  // Frontlight rows (X4 Pro): cycle in place and light the panel immediately.
  if (halFrontlight.present()) {
    items.push_back({MenuAction::FRONTLIGHT_BRIGHTNESS, StrId::STR_FRONTLIGHT_BRIGHTNESS});
    if (halFrontlight.hasWarmth()) {
      items.push_back({MenuAction::FRONTLIGHT_WARMTH, StrId::STR_FRONTLIGHT_WARMTH});
    }
  }
#endif
  items.push_back({MenuAction::FONT_LAYOUT, StrId::STR_FONT_LAYOUT_PREVIEW});
  items.push_back({MenuAction::READER_CONTROLS, StrId::STR_READER_CONTROLS});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  // Bookmarks / clippings / sync rows can be hidden by users who bind these
  // functions to reader controls instead (Settings > Reader).
  if (SETTINGS.readerMenuBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
    items.push_back({MenuAction::ADD_BOOKMARK, StrId::STR_CREATE_BOOKMARK});
  }
  if (SETTINGS.readerMenuClippings) {
    items.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
    items.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  // Screenshot is a developer/testing action — only surface it in Dev Mode.
  if (SETTINGS.devMode) {
    items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  }
  // Bluetooth remote toggle: only shown once a remote has been paired
  // (pairing itself lives in Settings > Bluetooth Page Turner), and hideable
  // like the bookmarks/clippings/sync rows (Settings > Reader).
  if (SETTINGS.readerMenuBluetooth && SETTINGS.bleBondedDeviceAddr[0] != '\0') {
    items.push_back({MenuAction::TOGGLE_BLUETOOTH, StrId::STR_BT_REMOTE_TOGGLE});
  }
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  // Sync rows. Gated on the backend that would actually handle a Push/Pull for
  // THIS book (ProgressAutoSync::providerFor, resolved by the reader and passed
  // in) — not just on the "Sync in Menu" toggle. A book with no linked backend
  // has nothing to sync to, and the reader's SYNC_PUSH/SYNC_PULL handler simply
  // falls out of its if/else and returns: the rows used to be shown anyway and
  // did nothing at all when pressed, with no error and no log line.
  //
  // Naming the destination also removes the guesswork about WHICH service a
  // generic "Push Local Progress" was about to talk to. Autosync rides the same
  // gate: it resolves its provider through the identical predicate, so it can't
  // fire for this book either. Its mode is still reachable in Settings > Reader,
  // and pendingAutosyncMode is seeded from SETTINGS regardless of whether the
  // row exists, so hiding it never writes the setting back.
  if (SETTINGS.readerMenuSync && syncProvider != ProgressAutoSync::Provider::None) {
    const bool isBookFusion = (syncProvider == ProgressAutoSync::Provider::BookFusion);
    items.push_back({MenuAction::AUTOSYNC, StrId::STR_AUTOSYNC});
    items.push_back(
        {MenuAction::SYNC_PUSH, isBookFusion ? StrId::STR_SYNC_PUSH_BOOKFUSION : StrId::STR_SYNC_PUSH_KOREADER});
    items.push_back(
        {MenuAction::SYNC_PULL, isBookFusion ? StrId::STR_SYNC_PULL_BOOKFUSION : StrId::STR_SYNC_PULL_KOREADER});
  }
  // Delete Book Cache is a testing aid — only surface it in Dev Mode.
  if (SETTINGS.devMode) {
    items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  }
  // Go Home sits last so it's always the final entry in the menu.
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::activateSelectedItem() {
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

  if (selectedAction == MenuAction::BUTTON_HINTS) {
    // Cycle the hint mode preview locally; applied on menu exit.
    pendingButtonHints = (pendingButtonHints + 1) % CrossPointSettings::BUTTON_HINTS_MODE_COUNT;
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::AUTOSYNC) {
    // Cycle the autosync mode preview locally; applied on menu exit.
    pendingAutosyncMode = (pendingAutosyncMode + 1) % CrossPointSettings::AUTOSYNC_COUNT;
    requestUpdate();
    return;
  }

  if (selectedAction == MenuAction::FRONTLIGHT_BRIGHTNESS || selectedAction == MenuAction::FRONTLIGHT_WARMTH) {
    // Cycle in 10% steps and drive the light immediately so the user can see
    // the level; the reader persists the values once, on menu exit.
    uint8_t& pending =
        (selectedAction == MenuAction::FRONTLIGHT_BRIGHTNESS) ? pendingFrontlightBrightness : pendingFrontlightWarmth;
    // Values are multiples of 10 from the UI, but the web API accepts any
    // 0-100 — cap the step at 100 before wrapping so odd values still cycle.
    pending = (pending >= 100) ? 0 : (pending + 10 > 100 ? 100 : pending + 10);
    halFrontlight.apply(pendingFrontlightBrightness, pendingFrontlightWarmth);
    requestUpdate();
    return;
  }

  setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption, pendingButtonHints,
                       pendingAutosyncMode, pendingFrontlightBrightness, pendingFrontlightWarmth});
  finish();
}

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

#if FREEINK_DEVICE_X4PRO
  // Full Touch: a vertical swipe turns a page of the menu, same as the
  // paginated list screens (inert while the whole menu fits on one page).
  if (TouchListNav::pageSwipe(mappedInput, static_cast<int>(menuItems.size()), visibleMenuRows(), selectedIndex)) {
    requestUpdate();
    return;
  }

  // Full Touch: first tap on a row moves the cursor; a second tap on the
  // selected row runs its Confirm action (cycling rows cycle in place).
  if (SETTINGS.fullTouchUi) {
    int lx, ly;
    if (mappedInput.wasTapPoint(lx, ly)) {
      const int top = menuTopY();
      const int rowsPerPage = visibleMenuRows();
      const int rowInPage = (ly - top) / MENU_ROW_H;
      // Taps address the drawn window: offset by the page the selection is on
      // (the same window render() draws, so paint and touch cannot disagree).
      const int row = selectedIndex / rowsPerPage * rowsPerPage + rowInPage;
      if (ly >= top && rowInPage >= 0 && rowInPage < rowsPerPage && row < static_cast<int>(menuItems.size())) {
        if (row != selectedIndex) {
          selectedIndex = row;
          requestUpdate();
        } else {
          activateSelectedItem();
        }
        return;
      }
      // Dead space (title/summary area): no action.
    }
  }
#endif

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedItem();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1,
                             pendingOrientation,
                             selectedPageTurnOption,
                             pendingButtonHints,
                             pendingAutosyncMode,
                             pendingFrontlightBrightness,
                             pendingFrontlightWarmth};
    setResult(std::move(result));
    finish();
    return;
  }
}

int EpubReaderMenuActivity::menuTopY() const {
  // Mirrors render()'s vertical flow: title block (45 + inverted-portrait
  // gutter), summary line, status line, 10px gap. Any change to those offsets
  // in render() must be reflected here or taps land on the wrong row.
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int contentY = isPortraitInverted ? 50 : 0;
  const int summaryLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int summaryY = 45 + contentY;
  const int statusY = summaryY + summaryLineHeight + 2;
  return statusY + summaryLineHeight + 10;
}

int EpubReaderMenuActivity::visibleMenuRows() const {
  // Mirrors render()'s vertical layout: the hint bar occupies the bottom edge
  // only in upright portrait — landscape puts hints in a side gutter and
  // inverted portrait reserves the top inside menuTopY().
  const int bottomReserve = renderer.getOrientation() == GfxRenderer::Orientation::Portrait
                                ? UITheme::getInstance().getMetrics().buttonHintsHeight
                                : 0;
  const int usable = renderer.getScreenHeight() - menuTopY() - bottomReserve;
  return usable >= MENU_ROW_H ? usable / MENU_ROW_H : 1;
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
  if (SETTINGS.readingSpeedSecondsPerPage > 0) {
    char speedBuf[16];
    const unsigned spp = SETTINGS.readingSpeedSecondsPerPage;
    if (spp < 60) {
      snprintf(speedBuf, sizeof(speedBuf), "%us/pg", spp);
    } else {
      snprintf(speedBuf, sizeof(speedBuf), "%um%us/pg", spp / 60, spp % 60);
    }
    progressLine += speedBuf;
    progressLine += " | ";
  }
  if (totalPages > 0) {
    progressLine += "Ch " + std::to_string(currentPage) + "/" + std::to_string(totalPages);
    if (timeLeftChapterSeconds > 0) {
      progressLine += " " + formatMins(timeLeftChapterSeconds);
    }
    progressLine += " | ";
  }
  progressLine += "Book " + std::to_string(bookProgressPercent) + "%";
  if (timeLeftBookSeconds > 0) {
    progressLine += " " + formatMins(timeLeftBookSeconds);
  }

  const int summaryY = 45 + contentY;
  renderer.drawCenteredText(UI_10_FONT_ID, summaryY, progressLine.c_str());

  // Status line: clock + date (hidden while the clock has never been set —
  // getCurrentValidTimestamp() returns 0 then) and today's reading vs the
  // daily goal. Past the goal it keeps showing actual/goal (e.g. "17m / 15m").
  std::string statusLine;
  const uint32_t nowTs = TimeUtils::getCurrentValidTimestamp();
  const std::string clockText = TimeUtils::formatTime(nowTs);
  if (!clockText.empty()) {
    statusLine += clockText + " " + TimeUtils::formatShortDate(nowTs) + " | ";
  }
  // Autosync needs the boot NTP check to have succeeded (that's how the
  // firmware knows WiFi works this session). When it didn't, autosync is
  // silently inactive — say so rather than leaving the user to wonder.
  if (SETTINGS.autosyncMode != CrossPointSettings::AUTOSYNC_OFF && !TimeUtils::wasTimeSyncedThisBoot()) {
    statusLine += tr(STR_AUTOSYNC_OFF_NOTICE);
    statusLine += " | ";
  }
  statusLine += ReadingStatsAnalytics::formatDurationHm(READING_STATS.getTodayReadingMs()) + " / " +
                ReadingStatsAnalytics::formatDurationHm(SETTINGS.getDailyGoalMs());

  const int summaryLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int statusY = summaryY + summaryLineHeight + 2;
  renderer.drawCenteredText(UI_10_FONT_ID, statusY, statusLine.c_str());

  // Menu Items (menuTopY() == statusY + summaryLineHeight + 10; shared with
  // the tap hit-testing so paint and touch can never disagree). The list pages
  // by visibleMenuRows(): the window containing the selection is drawn, and
  // Up/Down wrap (or a Full Touch vertical swipe) walks through the pages.
  const int startY = menuTopY();
  constexpr int lineHeight = MENU_ROW_H;
  const int rowsPerPage = visibleMenuRows();
  const size_t pageStart = static_cast<size_t>(selectedIndex / rowsPerPage * rowsPerPage);
  const size_t pageEnd = menuItems.size() < pageStart + rowsPerPage ? menuItems.size() : pageStart + rowsPerPage;

  if (menuItems.size() > static_cast<size_t>(rowsPerPage)) {
    // Page indicator, right-aligned on the status line: the hidden rows are
    // otherwise invisible on a menu this long.
    char pageBuf[8];
    const int totalPages = (static_cast<int>(menuItems.size()) + rowsPerPage - 1) / rowsPerPage;
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", selectedIndex / rowsPerPage + 1, totalPages);
    const auto pageW = renderer.getTextWidth(UI_10_FONT_ID, pageBuf);
    renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - pageW, statusY, pageBuf);
  }

  for (size_t i = pageStart; i < pageEnd; ++i) {
    const int displayY = startY + static_cast<int>(i - pageStart) * lineHeight;
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

    if (menuItems[i].action == MenuAction::BUTTON_HINTS) {
      const char* value = I18N.get(buttonHintsLabels[pendingButtonHints]);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }

    if (menuItems[i].action == MenuAction::DARK_MODE) {
      const char* value = SETTINGS.darkMode ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }

    if (menuItems[i].action == MenuAction::TOGGLE_BLUETOOTH) {
      // Show WHICH remote is active rather than a bare "On".
      std::string value;
      if (BluetoothHIDManager::getInstance().isEnabled()) {
        value = SETTINGS.bleBondedDeviceName[0]
                    ? renderer.truncatedText(UI_10_FONT_ID, SETTINGS.bleBondedDeviceName, 160)
                    : tr(STR_STATE_ON);
      } else {
        value = tr(STR_STATE_OFF);
      }
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value.c_str(), !isSelected);
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

    if (menuItems[i].action == MenuAction::AUTOSYNC) {
      const char* value = I18N.get(autosyncLabels[pendingAutosyncMode]);
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
    }

    if (menuItems[i].action == MenuAction::FRONTLIGHT_BRIGHTNESS ||
        menuItems[i].action == MenuAction::FRONTLIGHT_WARMTH) {
      const uint8_t pct = (menuItems[i].action == MenuAction::FRONTLIGHT_BRIGHTNESS) ? pendingFrontlightBrightness
                                                                                     : pendingFrontlightWarmth;
      char valueBuf[16];  // fits "100%" and translated "Off" (UTF-8)
      if (menuItems[i].action == MenuAction::FRONTLIGHT_BRIGHTNESS && pct == 0) {
        snprintf(valueBuf, sizeof(valueBuf), "%s", tr(STR_STATE_OFF));
      } else {
        snprintf(valueBuf, sizeof(valueBuf), "%u%%", pct);
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
