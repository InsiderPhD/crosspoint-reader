#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    BUTTON_HINTS,
    FRONTLIGHT_BRIGHTNESS,
    FRONTLIGHT_WARMTH,
    FONT_LAYOUT,
    READER_CONTROLS,
    BOOKMARKS,
    ADD_BOOKMARK,
    SAVE_CLIPPING,
    VIEW_CLIPPINGS,
    SCREENSHOT,
    TOGGLE_BLUETOOTH,
    DISPLAY_QR,
    GO_HOME,
    AUTOSYNC,
    SYNC_PUSH,
    SYNC_PULL,
    DELETE_CACHE
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool hasFootnotes,
                                  const uint32_t timeLeftChapterSeconds = 0, const uint32_t timeLeftBookSeconds = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handlesDirectTouch() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasFootnotes);

  // Menu row pitch; row i spans [menuTopY() + i*MENU_ROW_H, + MENU_ROW_H).
  // Shared by render() and the Full Touch tap hit-testing in loop().
  static constexpr int MENU_ROW_H = 30;
  int menuTopY() const;
  // The Confirm action for the selected row (in-place cycling rows cycle;
  // everything else closes the menu with a MenuResult). Also fired by a Full
  // Touch tap on the selected row.
  void activateSelectedItem();

  // Fixed menu layout
  const std::vector<MenuItem> menuItems;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  uint8_t pendingButtonHints = 0;
  uint8_t pendingAutosyncMode = 0;
  // Frontlight rows cycle these and drive the hardware live so the user sees
  // the level change; the reader persists them on menu exit.
  uint8_t pendingFrontlightBrightness = 0;
  uint8_t pendingFrontlightWarmth = 50;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<StrId> buttonHintsLabels = {StrId::STR_NONE_OPT, StrId::STR_SHORT_PRESS, StrId::STR_LONG_PRESS,
                                                StrId::STR_HINTS_FRONT_SHORT, StrId::STR_HINTS_FRONT_LONG};
  // Mirrors the Settings > Reader "Progress Autosync" enum order (STR_NONE_OPT is
  // the Off entry). Kept in sync with CrossPointSettings::AUTOSYNC.
  const std::vector<StrId> autosyncLabels = {StrId::STR_NONE_OPT, StrId::STR_AUTOSYNC_CHAPTER, StrId::STR_AUTOSYNC_5PCT,
                                             StrId::STR_AUTOSYNC_10PCT, StrId::STR_AUTOSYNC_ON_EXIT};
  const std::vector<const char*> pageTurnLabels = {
      I18N.get(StrId::STR_STATE_OFF), "Auto", "60s", "50s", "40s", "35s", "30s", "25s", "20s"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  uint32_t timeLeftChapterSeconds = 0;
  uint32_t timeLeftBookSeconds = 0;
};
