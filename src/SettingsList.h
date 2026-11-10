#pragma once

#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>

#include <vector>

#include "CrossPointSettings.h"
#include "HardcoverTokenStore.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
//
// The list is never stored: forEachSetting() builds one entry at a time and hands
// it to a visitor. SettingInfo is large (~176B: four std::function members plus an
// enum-values vector), so keeping all ~100 entries cost ~17KB of DRAM for the
// whole run, and a single braced initializer of them all materialized >8KB of
// temporaries in one stack frame (more than loopTask's 8KB stack). The helpers
// below push one entry per statement so only one temporary is live at a time.
namespace SettingsListDetail {

// The theme picker lists positions, not stored values: LYRA_LIBRARY is retired
// but keeps its persisted number, so the list skips it rather than renumbering
// the themes after it. Position i shows kUiThemeOptions[i].
constexpr uint8_t kUiThemeOptions[] = {CrossPointSettings::CLASSIC,       CrossPointSettings::LYRA,
                                       CrossPointSettings::LYRA_3_COVERS, CrossPointSettings::LYRA_CAROUSEL,
                                       CrossPointSettings::DASHBOARD,     CrossPointSettings::BOOKSHELF,
                                       CrossPointSettings::LICORICE};

inline uint8_t getUiThemeIndex() {
  const uint8_t theme =
      SETTINGS.uiTheme == CrossPointSettings::LYRA_LIBRARY ? CrossPointSettings::LYRA_3_COVERS : SETTINGS.uiTheme;
  for (uint8_t i = 0; i < sizeof(kUiThemeOptions); i++) {
    if (kUiThemeOptions[i] == theme) return i;
  }
  return 1;  // LYRA, the struct default
}

inline void setUiThemeIndex(const uint8_t index) {
  if (index < sizeof(kUiThemeOptions)) SETTINGS.uiTheme = kUiThemeOptions[index];
}

inline uint8_t getLongPressActionIndex() {
  switch (SETTINGS.longPressAction) {
    case CrossPointSettings::LONG_PRESS_SYNC:
      return 0;
    case CrossPointSettings::LONG_PRESS_PAGE_TURN:
      return 1;
    case CrossPointSettings::LONG_PRESS_REFRESH:
    case CrossPointSettings::LONG_PRESS_NONE:  // legacy value: "none" no longer exposed in UI
      return 2;
    case CrossPointSettings::LONG_PRESS_SLEEP:
      return 3;
    case CrossPointSettings::LONG_PRESS_BOOKMARK:
      return 4;
    default:
      return 2;
  }
}

inline void setLongPressActionIndex(const uint8_t index) {
  switch (index) {
    case 0:
      SETTINGS.longPressAction = CrossPointSettings::LONG_PRESS_SYNC;
      break;
    case 1:
      SETTINGS.longPressAction = CrossPointSettings::LONG_PRESS_PAGE_TURN;
      break;
    case 2:
      SETTINGS.longPressAction = CrossPointSettings::LONG_PRESS_REFRESH;
      break;
    case 3:
      SETTINGS.longPressAction = CrossPointSettings::LONG_PRESS_SLEEP;
      break;
    case 4:
      SETTINGS.longPressAction = CrossPointSettings::LONG_PRESS_BOOKMARK;
      break;
    default:
      break;
  }
}

inline uint8_t getShortPowerActionIndex() {
  switch (SETTINGS.shortPwrBtn) {
    case CrossPointSettings::PAGE_TURN:
      return 1;
    case CrossPointSettings::SLEEP:
      return 3;
    case CrossPointSettings::FORCE_REFRESH:
      return 2;
    case CrossPointSettings::SHORT_PWRBTN_SYNC:
      return 0;
    case CrossPointSettings::SHORT_PWRBTN_BOOKMARK:
      return 4;
    default:
      return 1;
  }
}

inline void setShortPowerActionIndex(const uint8_t index) {
  switch (index) {
    case 0:
      SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN_SYNC;
      break;
    case 1:
      SETTINGS.shortPwrBtn = CrossPointSettings::PAGE_TURN;
      break;
    case 2:
      SETTINGS.shortPwrBtn = CrossPointSettings::FORCE_REFRESH;
      break;
    case 3:
      SETTINGS.shortPwrBtn = CrossPointSettings::SLEEP;
      break;
    case 4:
      SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN_BOOKMARK;
      break;
    default:
      break;
  }
}

// Receives each entry as the helpers below build it and hands it straight to a
// visitor; nothing is retained. The helpers push one entry per statement, so
// only one ~176B SettingInfo temporary is live at a time.
class SettingSink {
 public:
  using Visit = void (*)(void* context, const SettingInfo& info);
  SettingSink(const Visit visit, void* context) : visit(visit), context(context) {}
  void push_back(const SettingInfo& info) {
    count++;
    visit(context, info);
  }
  size_t size() const { return count; }

 private:
  Visit visit;
  void* context;
  size_t count = 0;
};

// --- Display ---
inline void appendDisplaySettings(SettingSink& v) {
#if FREEINK_CAP_FRONTLIGHT
  // Frontlight boards only (compiled out elsewhere, so the JSON keys don't
  // round-trip on hardware that can never drive them). Warmth additionally
  // needs the second (warm) PWM channel.
  // Step 1 is the web slider's granularity, not the device's: the on-device row
  // cycles the FrontlightLevels ladder instead, so the dim end stays reachable
  // without 100 taps. Any 0-100 the web sets still cycles correctly from there.
  v.push_back(SettingInfo::Value(StrId::STR_FRONTLIGHT_BRIGHTNESS, &CrossPointSettings::frontlightBrightness,
                                 {0, 100, 1}, "frontlightBrightness", StrId::STR_CAT_DISPLAY));
#if FREEINK_CAP_WARMLIGHT
  v.push_back(SettingInfo::Value(StrId::STR_FRONTLIGHT_WARMTH, &CrossPointSettings::frontlightWarmth, {0, 100, 10},
                                 "frontlightWarmth", StrId::STR_CAT_DISPLAY));
#endif
#endif
  // The sleep theme folder; picked on the device from the installed themes
  // (SettingsActivity's Sleep Theme row) or on the web UI's Sleep Themes page.
  v.push_back(SettingInfo::String(StrId::STR_SLEEP_THEME, SETTINGS.sleepTheme, sizeof(SETTINGS.sleepTheme),
                                  "sleepTheme", StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Enum(StrId::STR_SEAMLESS_SLEEP, &CrossPointSettings::seamlessSleepScreen,
                                {StrId::STR_NEVER, StrId::STR_AFTER_TIMEOUT, StrId::STR_ALWAYS}, "seamlessSleepScreen",
                                StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Toggle(StrId::STR_SLEEP_FULL_REFRESH, &CrossPointSettings::sleepFullRefresh,
                                  "sleepFullRefresh", StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Enum(
      StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
      {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
      "refreshFrequency", StrId::STR_CAT_DISPLAY));
  // Persisted directly in JsonSettingsIO: a DynamicEnum has no valuePtr.
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_UI_THEME,
      {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED, StrId::STR_THEME_LYRA_CAROUSEL,
       StrId::STR_THEME_DASHBOARD, StrId::STR_THEME_BOOKSHELF, StrId::STR_THEME_LICORICE},
      getUiThemeIndex, setUiThemeIndex, "uiTheme", StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                                  StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::darkMode, "darkMode",
                                  StrId::STR_CAT_DISPLAY));
}

// --- Reader: fonts and layout ---
inline void appendReaderTypographySettings(SettingSink& v) {
  v.push_back(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                                {StrId::STR_BOOKERLY, StrId::STR_INTER, StrId::STR_OPEN_DYSLEXIC, StrId::STR_MONOSPACE},
                                "fontFamily", StrId::STR_CAT_READER));
  // SD card font family name (persisted only; selection happens in FontSelectionActivity
  // launched by the FontFamily action below). Hidden from device UI (no category) and from
  // web UI categorization, but round-trips through settings.json.
  v.push_back(SettingInfo::String(StrId::STR_FONT_FAMILY, CrossPointSettings::getInstance().sdFontFamilyName,
                                  sizeof(CrossPointSettings::getInstance().sdFontFamilyName), "sdFontFamilyName"));
  // Dictionary folder name (persisted only; selection happens in
  // DictionarySelectActivity, launched by the Dictionary action). Hidden
  // from the device UI (no category) like sdFontFamilyName above, but
  // round-trips through settings.json and the web UI.
  v.push_back(SettingInfo::String(StrId::STR_DICTIONARY, CrossPointSettings::getInstance().dictionaryName,
                                  sizeof(CrossPointSettings::getInstance().dictionaryName), "dictionaryName"));
  v.push_back(SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                                {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE}, "fontSize",
                                StrId::STR_CAT_READER));
  // "lineHeight", CrossPoint 2.0's key and numbering. JsonSettingsIO reads an older file's
  // "lineSpacing" (Tight / Normal / Wide) one step up when "lineHeight" is absent.
  v.push_back(SettingInfo::Enum(
      StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
      {StrId::STR_SPACING_0_PERCENT, StrId::STR_SPACING_PLUS_10_PERCENT, StrId::STR_SPACING_PLUS_20_PERCENT_DEFAULT,
       StrId::STR_SPACING_PLUS_30_PERCENT, StrId::STR_SPACING_PLUS_45_PERCENT, StrId::STR_SPACING_PLUS_60_PERCENT,
       StrId::STR_SPACING_PLUS_75_PERCENT},
      "lineHeight", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Value(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacing,
                                 {CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX,
                                  CrossPointSettings::WORD_SPACING_STEP},
                                 "wordSpacing", StrId::STR_CAT_READER));
  // Order is the persisted index: -1.5 px .. +1.5 px in half-pixel steps.
  v.push_back(SettingInfo::Enum(
      StrId::STR_CHARACTER_SPACING, &CrossPointSettings::characterSpacing,
      {StrId::STR_SPACING_MINUS_3, StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1, StrId::STR_SPACING_ZERO,
       StrId::STR_SPACING_PLUS_1, StrId::STR_SPACING_PLUS_2, StrId::STR_SPACING_PLUS_3},
      "characterSpacing", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5},
                                 "screenMargin", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Enum(
      StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
      {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
      "paragraphAlignment", StrId::STR_CAT_READER));
  // Order is the persisted index (TextLayoutOptions::PARAGRAPH_INDENT_*): the book's
  // own indent, none, then 1..4 word spaces.
  v.push_back(SettingInfo::Enum(StrId::STR_PARAGRAPH_INDENT, &CrossPointSettings::paragraphIndent,
                                {StrId::STR_BOOK_S_STYLE, StrId::STR_NONE_OPT, StrId::STR_SMALL, StrId::STR_MEDIUM,
                                 StrId::STR_LARGE, StrId::STR_X_LARGE},
                                "paragraphIndent", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                  StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled, "hyphenationEnabled",
                                  StrId::STR_CAT_READER));
  v.push_back(
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER));
}

// --- Reader: rendering behaviour and menu ---
inline void appendReaderBehaviourSettings(SettingSink& v) {
  v.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                  "extraParagraphSpacing", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                                  StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                                {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                                "imageRendering", StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_IMAGE_GRAYSCALE, &CrossPointSettings::imageGrayscale, "imageGrayscale",
                                  StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Enum(StrId::STR_FOOTNOTES, &CrossPointSettings::footnoteDisplay,
                                {StrId::STR_FOOTNOTE_ON_PAGE, StrId::STR_FOOTNOTE_IN_MENU}, "footnoteDisplay",
                                StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReading, "bionicReading",
                                  StrId::STR_CAT_READER));
  v.push_back(SettingInfo::Enum(StrId::STR_SHOW_BUTTON_HINTS, &CrossPointSettings::showButtonHints,
                                {StrId::STR_NONE_OPT, StrId::STR_SHORT_PRESS, StrId::STR_LONG_PRESS,
                                 StrId::STR_HINTS_FRONT_SHORT, StrId::STR_HINTS_FRONT_LONG},
                                "showButtonHints", StrId::STR_CAT_READER));
  // Silent background progress push. Coexists with a Bluetooth remote:
  // when a push fires the BLE stack is torn down for the WiFi session and
  // auto-restored afterwards (see EpubReaderActivity::runAutosyncNow).
  v.push_back(SettingInfo::Enum(StrId::STR_AUTOSYNC, &CrossPointSettings::autosyncMode,
                                {StrId::STR_NONE_OPT, StrId::STR_AUTOSYNC_CHAPTER, StrId::STR_AUTOSYNC_5PCT,
                                 StrId::STR_AUTOSYNC_10PCT, StrId::STR_AUTOSYNC_ON_EXIT},
                                "autosyncMode", StrId::STR_CAT_READER));
}

// --- Stats tab ---
inline void appendStatsSettings(SettingSink& v) {
  v.push_back(SettingInfo::Toggle(StrId::STR_TRACK_READING_STATS, &CrossPointSettings::trackReadingStats,
                                  "trackReadingStats", StrId::STR_CAT_STATS));
  v.push_back(
      SettingInfo::Enum(StrId::STR_DAILY_GOAL, &CrossPointSettings::dailyReadingGoal,
                        {StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30, StrId::STR_MIN_60},
                        "dailyReadingGoal", StrId::STR_CAT_STATS));
  v.push_back(SettingInfo::Enum(StrId::STR_MIN_SESSION_THRESHOLD, &CrossPointSettings::minSessionMinutes,
                                {StrId::STR_MIN_1, StrId::STR_MIN_3, StrId::STR_MIN_5}, "minSessionMinutes",
                                StrId::STR_CAT_STATS));
  // Contact string for "if found" sleep themes (device.contact), e.g. a phone number.
  v.push_back(SettingInfo::String(StrId::STR_RETURN_CONTACT, SETTINGS.returnContact, sizeof(SETTINGS.returnContact),
                                  "returnContact", StrId::STR_CAT_STATS));
  // RETIRED time-zone preset (index into the old 29-zone list), superseded by
  // clockTimezone, which JsonSettingsIO persists by hand. No category, so it
  // is hidden everywhere; listed only so settings.json keeps round-tripping
  // it for older firmware and for TimeZoneRegistry::resolve()'s migration.
  v.push_back(
      SettingInfo::Value(StrId::STR_TIME_ZONE, &CrossPointSettings::timeZonePreset, {0, 28, 1}, "timeZonePreset"));
}

// --- Controls ---
inline void appendControlSettings(SettingSink& v) {
  static const std::vector<StrId> sharedReaderButtonActions = {
      StrId::STR_SYNC_WITH_BOOKFUSION, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_SLEEP,
      StrId::STR_CREATE_BOOKMARK,
  };

  v.push_back(SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION,
                                  &CrossPointSettings::frontButtonFollowOrientation,
                                  "frontButtonFollowOrientation"
#if !FREEINK_CAP_TOUCH
                                  // No front buttons on X4 Pro: keep the JSON key round-tripping
                                  // but hide the toggle (no category = hidden from UI).
                                  ,
                                  StrId::STR_CAT_SYSTEM
#endif
                                  ));
  v.push_back(SettingInfo::Toggle(StrId::STR_YOLO_SELECTION, &CrossPointSettings::yoloSelection,
                                  "yoloSelection"
#if FREEINK_CAP_TOUCH
                                  // Only meaningful under Full Touch, which is X4 Pro only.
                                  ,
                                  StrId::STR_CAT_SYSTEM
#endif
                                  ));
  // Legacy settings: kept for JSON round-trip / migration only (no category = hidden from UI).
  v.push_back(SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                                {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout"));
  v.push_back(SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip,
                                  "longPressChapterSkip"));
  v.push_back(SettingInfo::DynamicEnum(StrId::STR_LONG_PRESS_ACTION, sharedReaderButtonActions, getLongPressActionIndex,
                                       setLongPressActionIndex, "longPressAction"));
  v.push_back(SettingInfo::DynamicEnum(StrId::STR_SHORT_PWR_BTN, sharedReaderButtonActions, getShortPowerActionIndex,
                                       setShortPowerActionIndex, "shortPwrBtn"));
  // Tilt page turn (QMI8658 IMU, X3). Always listed so settings.json keeps the
  // saved value when the IMU probe fails on one boot; forEachUiSetting() hides
  // the row while no IMU answers.
  v.push_back(SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED}, "tiltPageTurn",
                                StrId::STR_CAT_SYSTEM));
  v.push_back(SettingInfo::Toggle(StrId::STR_TILT_NEXT_PAGE_ONLY, &CrossPointSettings::tiltNextPageOnly,
                                  "tiltNextPageOnly", StrId::STR_CAT_SYSTEM));
  // Session switch flipped by the Disable Page-Turn Gestures reader action.
  // No category: persisted (and round-tripped on every board) but not a row.
  v.push_back(SettingInfo::Toggle(StrId::STR_DISABLE_PAGE_TURN_GESTURES, &CrossPointSettings::disablePageTurnGestures,
                                  "disablePageTurnGestures"));
  // Per-button reader actions are persisted directly in JsonSettingsIO (not via SettingsList)
  // to allow proper clamping against READER_ACTION_COUNT.
  // Persisted last-chosen sort. Hidden from the settings UI (no category set) —
  // edited only via the in-activity sort menu. Listed here so JsonSettingsIO
  // round-trips it through settings.json. The enumValues just exist for
  // validation clamping in loadSettings; users never see them.
  v.push_back(
      SettingInfo::Enum(StrId::STR_SORT_BY, &CrossPointSettings::sortMode,
                        {StrId::STR_SORT_ALPHA_ASC, StrId::STR_SORT_ALPHA_DESC, StrId::STR_SORT_AUTHOR_ASC,
                         StrId::STR_SORT_AUTHOR_DESC, StrId::STR_SORT_LAST_OPENED_NEW, StrId::STR_SORT_LAST_OPENED_OLD,
                         StrId::STR_SORT_PROGRESS_MOST, StrId::STR_SORT_PROGRESS_LEAST, StrId::STR_SORT_DATE_ADDED_NEW,
                         StrId::STR_SORT_DATE_ADDED_OLD, StrId::STR_SORT_BOOKFUSION_FIRST,
                         StrId::STR_SORT_BOOKFUSION_LAST, StrId::STR_SORT_TAG_ASC, StrId::STR_SORT_TAG_DESC},
                        "sortMode"));
  // Grouped-view (Tags/Authors/Series) sort. Hidden from the settings UI like
  // sortMode — edited only via the in-activity sort menu. The 15 enumValues exist
  // only for load-time clamping (so SeriesIndexAsc=14 validates); the default
  // sentinel 0xFF is out of range and clamps back to itself ("unset").
  v.push_back(SettingInfo::Enum(
      StrId::STR_SORT_BY, &CrossPointSettings::groupSortMode,
      {StrId::STR_SORT_ALPHA_ASC, StrId::STR_SORT_ALPHA_DESC, StrId::STR_SORT_AUTHOR_ASC, StrId::STR_SORT_AUTHOR_DESC,
       StrId::STR_SORT_LAST_OPENED_NEW, StrId::STR_SORT_LAST_OPENED_OLD, StrId::STR_SORT_PROGRESS_MOST,
       StrId::STR_SORT_PROGRESS_LEAST, StrId::STR_SORT_DATE_ADDED_NEW, StrId::STR_SORT_DATE_ADDED_OLD,
       StrId::STR_SORT_BOOKFUSION_FIRST, StrId::STR_SORT_BOOKFUSION_LAST, StrId::STR_SORT_TAG_ASC,
       StrId::STR_SORT_TAG_DESC, StrId::STR_SORT_SERIES_ORDER},
      "groupSortMode"));
}

// --- System ---
inline void appendSystemSettings(SettingSink& v) {
  v.push_back(
      SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                        "sleepTimeout", StrId::STR_CAT_SYSTEM));
  v.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                                  StrId::STR_CAT_SYSTEM));
  v.push_back(SettingInfo::Enum(StrId::STR_MOVE_FINISHED_BOOKS, &CrossPointSettings::finishedBookFolder,
                                {StrId::STR_NEVER, StrId::STR_TO_READ_FOLDER, StrId::STR_TO_COMPLETED_FOLDER},
                                "finishedBookFolder", StrId::STR_CAT_SYSTEM));
  v.push_back(SettingInfo::Enum(StrId::STR_FOLDER_VIEW, &CrossPointSettings::folderView,
                                {StrId::STR_FOLDERS, StrId::STR_TAGS, StrId::STR_AUTHORS, StrId::STR_SERIES},
                                "folderView", StrId::STR_CAT_DISPLAY));
  // Library only: finished books (completed in the reader, or
  // marked as read) are left out of the grid.
  v.push_back(SettingInfo::Toggle(StrId::STR_HIDE_FINISHED_BOOKS, &CrossPointSettings::libraryHideFinished,
                                  "libraryHideFinished", StrId::STR_CAT_DISPLAY));
  v.push_back(SettingInfo::Toggle(StrId::STR_DEV_MODE, &CrossPointSettings::devMode, "devMode", StrId::STR_CAT_SYSTEM));
  // Plugins master switch. Persisted on every board (settings.json
  // round-trips with upstream); shown only where plugins are built. The device
  // Settings screen moves the row next to the Plugins list it gates.
  v.push_back(SettingInfo::Toggle(StrId::STR_ENABLE_PLUGINS, &CrossPointSettings::pluginsEnabled, "pluginsEnabled"
#if CROSSPOINT_SD_PLUGINS
                                  ,
                                  StrId::STR_CAT_SYSTEM
#endif
                                  ));
  // Dev tab: opting a device into beta/rc firmware. Lives behind Dev
  // Mode because the pre-release channel can hand this device an
  // unvalidated image, and the flasher deliberately bypasses
  // esp_image_verify (see OtaBootSwitch.h).
  v.push_back(SettingInfo::Toggle(StrId::STR_ENABLE_PRERELEASES, &CrossPointSettings::allowPreReleases,
                                  "allowPreReleases", StrId::STR_CAT_DEV));
  // Dev tab: the CLX1 Library index is on by default; this is the
  // switch back to the in-RAM path list for comparison on hardware.
  v.push_back(SettingInfo::Toggle(StrId::STR_LIBRARY_USE_INDEX, &CrossPointSettings::libraryUseIndex, "libraryUseIndex",
                                  StrId::STR_CAT_DEV));
}

// --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
// One custom header slot as two web fields. Saves only on a real change.
inline SettingInfo koCustomHeaderName(const size_t slot, const StrId label, const char* key) {
  return SettingInfo::DynamicString(
      label, [slot] { return KOREADER_STORE.getCustomHeaders()[slot].name; },
      [slot](const std::string& val) {
        const HttpHeader& current = KOREADER_STORE.getCustomHeaders()[slot];
        if (val == current.name) return;
        KOREADER_STORE.setCustomHeader(slot, val, current.value);
        KOREADER_STORE.saveToFile();
      },
      key, StrId::STR_KOREADER_SYNC);
}
inline SettingInfo koCustomHeaderValue(const size_t slot, const StrId label, const char* key) {
  return SettingInfo::DynamicString(
      label, [slot] { return KOREADER_STORE.getCustomHeaders()[slot].value; },
      [slot](const std::string& val) {
        const HttpHeader& current = KOREADER_STORE.getCustomHeaders()[slot];
        if (val == current.value) return;
        KOREADER_STORE.setCustomHeader(slot, current.name, val);
        KOREADER_STORE.saveToFile();
      },
      key, StrId::STR_KOREADER_SYNC);
}

inline void appendKoReaderSettings(SettingSink& v) {
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
      [](const std::string& val) {
        KOREADER_STORE.setCredentials(val, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
      [](const std::string& val) {
        KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), val);
        KOREADER_STORE.saveToFile();
      },
      "koPassword", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
      [](const std::string& val) {
        KOREADER_STORE.setServerUrl(val);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
      [](uint8_t val) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(val));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  // Option order is the enum's (CrossPoint, Standard KOSync, Extended).
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_KOSYNC_SERVER_TYPE,
      {StrId::STR_KOSYNC_TYPE_CROSSPOINT, StrId::STR_KOSYNC_TYPE_STANDARD, StrId::STR_KOSYNC_TYPE_EXTENDED},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getServerType()); },
      [](uint8_t val) {
        const auto type = static_cast<KOReaderServerType>(val);
        if (type == KOREADER_STORE.getServerType()) return;
        KOREADER_STORE.setServerType(type);
        KOREADER_STORE.saveToFile();
      },
      "koServerType", StrId::STR_KOREADER_SYNC));
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_SYNC_BEHAVIOR, {StrId::STR_ASK_EVERY_TIME, StrId::STR_SMART_SYNC},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](uint8_t val) {
        const auto behavior = static_cast<KOReaderSyncBehavior>(val);
        if (behavior == KOREADER_STORE.getSyncBehavior()) return;
        KOREADER_STORE.setSyncBehavior(behavior);
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  v.push_back(koCustomHeaderName(0, StrId::STR_KOREADER_CUSTOM_HEADER_1_NAME, "koCustomHeader1Name"));
  v.push_back(koCustomHeaderValue(0, StrId::STR_KOREADER_CUSTOM_HEADER_1_VALUE, "koCustomHeader1Value"));
  v.push_back(koCustomHeaderName(1, StrId::STR_KOREADER_CUSTOM_HEADER_2_NAME, "koCustomHeader2Name"));
  v.push_back(koCustomHeaderValue(1, StrId::STR_KOREADER_CUSTOM_HEADER_2_VALUE, "koCustomHeader2Value"));
  v.push_back(SettingInfo::DynamicEnum(
      StrId::STR_KOSYNC_SEND_METADATA, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON},
      [] { return static_cast<uint8_t>(KOREADER_STORE.getSendMetadata() ? 1 : 0); },
      [](uint8_t val) {
        if ((val != 0) == KOREADER_STORE.getSendMetadata()) return;
        KOREADER_STORE.setSendMetadata(val != 0);
        KOREADER_STORE.saveToFile();
      },
      "koSendMetadata", StrId::STR_KOREADER_SYNC));
}

// --- Hardcover (web-only, uses HardcoverTokenStore) ---
// The token is a JWT of several hundred characters: pasted here, never typed on
// the device. The getter never echoes it back. The web settings API serialises
// each setting into a 512-byte buffer and silently drops larger ones, so the
// real token would vanish from the page; a short placeholder stands in for it,
// and saving the placeholder unchanged is ignored. Clearing the field unlinks.
inline void appendHardcoverSettings(SettingSink& v) {
  v.push_back(SettingInfo::DynamicString(
      StrId::STR_HARDCOVER_API_TOKEN,
      [] { return HC_TOKEN_STORE.hasToken() ? std::string(tr(STR_HARDCOVER_TOKEN_SAVED)) : std::string(); },
      [](const std::string& val) {
        if (val == tr(STR_HARDCOVER_TOKEN_SAVED)) return;
        HC_TOKEN_STORE.setToken(val);
      },
      "hardcoverToken", StrId::STR_HARDCOVER));
}

// --- OPDS Browser (web-only) ---
// The catalogs themselves live in OpdsServerStore (/.crosspoint/opds.json). The
// old single-server opdsServerUrl/opdsUsername/opdsPassword fields are read and
// written by JsonSettingsIO directly, only so the store can migrate them.
inline void appendOpdsSettings(SettingSink& v) {
  v.push_back(SettingInfo::Toggle(StrId::STR_OPDS_SERVER_FILENAMES, &CrossPointSettings::opdsUseServerFilename,
                                  "opdsUseServerFilename", StrId::STR_OPDS_BROWSER));
  // Normalised where it is used (OpdsDownloadFolder::normalize), so a web
  // edit without the leading '/' still works.
  v.push_back(SettingInfo::String(StrId::STR_OPDS_DEFAULT_DOWNLOAD_FOLDER, SETTINGS.opdsDownloadFolder,
                                  sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder", StrId::STR_OPDS_BROWSER));
}

// --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
inline void appendStatusBarSettings(SettingSink& v) {
  // Settings both bars share.
  // Master hide, toggled from the reader via READER_ACTION_HIDE_STATUS_BAR;
  // listed here so it persists and is visible/reversible from the web UI.
  v.push_back(SettingInfo::Toggle(StrId::STR_HIDE_STATUS_BAR, &CrossPointSettings::statusBarHidden, "statusBarHidden",
                                  StrId::STR_CUSTOMISE_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_PERCENTAGE_FORMAT, &CrossPointSettings::statusBarPercentageFormat,
                                {StrId::STR_PERCENTAGE_FORMAT_WHOLE, StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                                 StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS},
                                "statusBarPercentageFormat", StrId::STR_CUSTOMISE_STATUS_BAR));
  // Font of both reader bars; taller text reserves a taller lane (reflows the book).
  v.push_back(SettingInfo::Enum(StrId::STR_STATUS_BAR_TEXT_SIZE, &CrossPointSettings::statusBarTextSize,
                                {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}, "statusBarTextSize",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  // XTC pages are pre-rendered, so the bars are overlays: which edges get one.
  v.push_back(SettingInfo::Enum(StrId::STR_XTC_STATUS_BAR, &CrossPointSettings::xtcStatusBarMode,
                                {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP, StrId::STR_BOTH},
                                "xtcStatusBarMode", StrId::STR_CUSTOMISE_STATUS_BAR));
  // Extra empty pixels reserved between the body text and each bar.
  v.push_back(SettingInfo::Value(
      StrId::STR_STATUS_BAR_TOP_MARGIN, &CrossPointSettings::statusBarTopMargin,
      {0, CrossPointSettings::STATUS_BAR_TOP_MARGIN_MAX, CrossPointSettings::STATUS_BAR_TOP_MARGIN_STEP},
      "statusBarTopMargin", StrId::STR_CUSTOMISE_STATUS_BAR));
  // RETIRED clock offset (0..104 quarter-hour steps, biased by 48), superseded
  // by clockTimezone. No category, so it is hidden from the web UI too; kept
  // in the list only so settings.json round-trips it (see timeZonePreset).
  v.push_back(SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                                 "clockUtcOffsetQ"));
  v.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                                {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                                StrId::STR_CUSTOMISE_STATUS_BAR));
  // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
  // on next WiFi connect, which is useful when crossing time zones.
  v.push_back(SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced,
                                  "clockHasBeenSynced", StrId::STR_CUSTOMISE_STATUS_BAR));
  // Bottom bar: every element's Hide / Left / Center / Right cluster, then the
  // bar's own fill bar. These replace the legacy show/hide toggles and
  // Book/Chapter enums.
  v.push_back(SettingInfo::Enum(StrId::STR_BATTERY, &CrossPointSettings::statusBarBatteryPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBatteryPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(
      StrId::STR_BATTERY_STYLE, &CrossPointSettings::statusBarBatteryStyle,
      {StrId::STR_BATTERY_ICON_AND_PERCENT, StrId::STR_BATTERY_ICON_ONLY, StrId::STR_BATTERY_PERCENT_ONLY},
      "statusBarBatteryStyle", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_SB_CENTERED, &CrossPointSettings::statusBarLayout,
                                {StrId::STR_STATE_OFF, StrId::STR_SB_CENTER_BULLET, StrId::STR_SB_CENTER_DOT,
                                 StrId::STR_SB_CENTER_DASH, StrId::STR_SB_CENTER_PIPE},
                                "statusBarLayout", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_TITLE, &CrossPointSettings::statusBarBookTitlePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBookTitlePos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_TITLE, &CrossPointSettings::statusBarChapterTitlePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarChapterTitlePos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookPercentPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBookPercentPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPagePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarChapterPagePos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarChapterPercentPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarChapterPercentPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_TIME_LEFT, &CrossPointSettings::statusBarBookTimeLeftPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBookTimeLeftPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_TIME_LEFT, &CrossPointSettings::statusBarChapterTimeLeftPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarChapterTimeLeftPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOKMARK, &CrossPointSettings::statusBarBookmarkPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBookmarkPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BLUETOOTH, &CrossPointSettings::statusBarBluetoothPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarBluetoothPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_DATE, &CrossPointSettings::statusBarDatePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarDatePos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarClockPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarClockPos", StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                                {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                                StrId::STR_BOTTOM_STATUS_BAR));
  v.push_back(
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                        {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                        "statusBarProgressBarThickness", StrId::STR_BOTTOM_STATUS_BAR));
  // Top bar: the same elements placed independently, with its own fill bar
  // along the top edge above the text lane.
  v.push_back(SettingInfo::Enum(StrId::STR_BATTERY, &CrossPointSettings::statusBarTopBatteryPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBatteryPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(
      StrId::STR_BATTERY_STYLE, &CrossPointSettings::statusBarTopBatteryStyle,
      {StrId::STR_BATTERY_ICON_AND_PERCENT, StrId::STR_BATTERY_ICON_ONLY, StrId::STR_BATTERY_PERCENT_ONLY},
      "statusBarTopBatteryStyle", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_SB_CENTERED, &CrossPointSettings::statusBarTopLayout,
                                {StrId::STR_STATE_OFF, StrId::STR_SB_CENTER_BULLET, StrId::STR_SB_CENTER_DOT,
                                 StrId::STR_SB_CENTER_DASH, StrId::STR_SB_CENTER_PIPE},
                                "statusBarTopLayout", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_TITLE, &CrossPointSettings::statusBarTopBookTitlePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBookTitlePos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_TITLE, &CrossPointSettings::statusBarTopChapterTitlePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopChapterTitlePos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarTopBookPercentPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBookPercentPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarTopChapterPagePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopChapterPagePos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_PROGRESS_PERCENTAGE,
                                &CrossPointSettings::statusBarTopChapterPercentPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopChapterPercentPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOK_TIME_LEFT, &CrossPointSettings::statusBarTopBookTimeLeftPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBookTimeLeftPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CHAPTER_TIME_LEFT, &CrossPointSettings::statusBarTopChapterTimeLeftPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopChapterTimeLeftPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BOOKMARK, &CrossPointSettings::statusBarTopBookmarkPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBookmarkPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_BLUETOOTH, &CrossPointSettings::statusBarTopBluetoothPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopBluetoothPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_DATE, &CrossPointSettings::statusBarTopDatePos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopDatePos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_CLOCK, &CrossPointSettings::statusBarTopClockPos,
                                {StrId::STR_HIDE, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                                "statusBarTopClockPos", StrId::STR_TOP_STATUS_BAR));
  v.push_back(SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarTopProgressBar,
                                {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTopProgressBar",
                                StrId::STR_TOP_STATUS_BAR));
  v.push_back(
      SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarTopProgressBarThickness,
                        {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                        "statusBarTopProgressBarThickness", StrId::STR_TOP_STATUS_BAR));
}

// Exact number of entries the append helpers emit. Nothing allocates from it any
// more (the list is visited, never stored); forEachSetting() logs a mismatch so a
// helper that stops emitting an entry (and so stops persisting it) shows up on
// the serial log. Recount when adding or removing an entry.
constexpr size_t SETTINGS_LIST_UNCONDITIONAL_COUNT = 107;  // includes tiltPageTurn, tiltNextPageOnly, pluginsEnabled, opdsDownloadFolder
#if FREEINK_CAP_FRONTLIGHT && FREEINK_CAP_WARMLIGHT
constexpr size_t SETTINGS_LIST_FRONTLIGHT_COUNT = 2;  // brightness + warmth
#elif FREEINK_CAP_FRONTLIGHT
constexpr size_t SETTINGS_LIST_FRONTLIGHT_COUNT = 1;  // brightness
#else
constexpr size_t SETTINGS_LIST_FRONTLIGHT_COUNT = 0;
#endif

}  // namespace SettingsListDetail

// Visits every setting synchronously, in list order. Nothing outlives the call:
// each entry is built, handed to `visit` and destroyed before the next one, so
// the list (~100 x ~176B, about 17KB on the C3) is never resident. It used to be
// a function-static vector pinned in DRAM for the whole run. The reference is
// valid only during the callback; copy the entry to keep it.
// Persistence (JsonSettingsIO load/save) walks this full list, so no key is ever
// dropped because of a runtime hardware probe.
void forEachSetting(SettingsListDetail::SettingSink::Visit visit, void* context);

template <typename Fn>
void forEachSetting(Fn& fn) {
  forEachSetting([](void* context, const SettingInfo& info) { (*static_cast<Fn*>(context))(info); }, &fn);
}

// The settings UI view (device screen and web page): forEachSetting() minus rows
// whose hardware did not answer on this boot. UI-only: a probe result must never
// reach persistence, or one failed probe would drop the saved value.
template <typename Fn>
void forEachUiSetting(Fn& fn) {
  const bool hasTiltSensor = halTiltSensor.isAvailable();
  auto filter = [&fn, hasTiltSensor](const SettingInfo& info) {
    // Tilt page turn needs the QMI8658 IMU (X3).
    if (!hasTiltSensor && (info.nameId == StrId::STR_TILT_PAGE_TURN || info.nameId == StrId::STR_TILT_NEXT_PAGE_ONLY)) {
      return;
    }
    fn(info);
  };
  forEachSetting(filter);
}
