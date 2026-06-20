#pragma once

#include <HalClock.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline const std::vector<SettingInfo>& getSettingsList() {
  static const std::vector<StrId> sharedReaderButtonActions = {
      StrId::STR_SYNC_WITH_BOOKFUSION, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH, StrId::STR_SLEEP,
      StrId::STR_CREATE_BOOKMARK,
  };

  auto getLongPressActionIndex = []() -> uint8_t {
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
  };

  auto setLongPressActionIndex = [](const uint8_t index) {
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
  };

  auto getShortPowerActionIndex = []() -> uint8_t {
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
  };

  auto setShortPowerActionIndex = [](const uint8_t index) {
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
  };

  static const std::vector<SettingInfo> list = [&] {
    std::vector<SettingInfo> v = {
        // --- Display ---
        SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                          {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                           StrId::STR_COVER_CUSTOM},
                          "sleepScreen", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                          {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                          {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                          "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_SEAMLESS_SLEEP, &CrossPointSettings::seamlessSleepScreen,
                          {StrId::STR_NEVER, StrId::STR_AFTER_TIMEOUT, StrId::STR_ALWAYS}, "seamlessSleepScreen",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                          {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                          StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(
            StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
            {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
            "refreshFrequency", StrId::STR_CAT_DISPLAY),
        SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                          {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                           StrId::STR_THEME_LYRA_LIBRARY},
                          "uiTheme", StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                            StrId::STR_CAT_DISPLAY),
        SettingInfo::Toggle(StrId::STR_READER_DARK_MODE, &CrossPointSettings::darkMode, "darkMode",
                            StrId::STR_CAT_DISPLAY),

        // --- Reader ---
        SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                          {StrId::STR_BOOKERLY, StrId::STR_INTER, StrId::STR_OPEN_DYSLEXIC, StrId::STR_MONOSPACE},
                          "fontFamily", StrId::STR_CAT_READER),
        // SD card font family name (persisted only; selection happens in FontSelectionActivity
        // launched by the FontFamily action below). Hidden from device UI (no category) and from
        // web UI categorization, but round-trips through settings.json.
        SettingInfo::String(StrId::STR_FONT_FAMILY, CrossPointSettings::getInstance().sdFontFamilyName,
                            sizeof(CrossPointSettings::getInstance().sdFontFamilyName), "sdFontFamilyName"),
        SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                          {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE}, "fontSize",
                          StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                          {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing", StrId::STR_CAT_READER),
        SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5}, "screenMargin",
                           StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                          {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                           StrId::STR_BOOK_S_STYLE},
                          "paragraphAlignment", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                          {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                          "orientation", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                            "extraParagraphSpacing", StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                          {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                          "imageRendering", StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_FOOTNOTES, &CrossPointSettings::footnoteDisplay,
                          {StrId::STR_FOOTNOTE_ON_PAGE, StrId::STR_FOOTNOTE_IN_MENU}, "footnoteDisplay",
                          StrId::STR_CAT_READER),
        SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReading, "bionicReading",
                            StrId::STR_CAT_READER),
        SettingInfo::Enum(StrId::STR_SHOW_BUTTON_HINTS, &CrossPointSettings::showButtonHints,
                          {StrId::STR_NONE_OPT, StrId::STR_SHORT_PRESS, StrId::STR_LONG_PRESS,
                           StrId::STR_HINTS_FRONT_SHORT, StrId::STR_HINTS_FRONT_LONG},
                          "showButtonHints", StrId::STR_CAT_READER),
        // --- Controls ---
        SettingInfo::Toggle(StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, &CrossPointSettings::frontButtonFollowOrientation,
                            "frontButtonFollowOrientation", StrId::STR_CAT_SYSTEM),
        // Legacy settings: kept for JSON round-trip / migration only (no category = hidden from UI).
        SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                          {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout"),
        SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip,
                            "longPressChapterSkip"),
        SettingInfo::DynamicEnum(StrId::STR_LONG_PRESS_ACTION, sharedReaderButtonActions, getLongPressActionIndex,
                                 setLongPressActionIndex, "longPressAction"),
        SettingInfo::DynamicEnum(StrId::STR_SHORT_PWR_BTN, sharedReaderButtonActions, getShortPowerActionIndex,
                                 setShortPowerActionIndex, "shortPwrBtn"),
        // Per-button reader actions are persisted directly in JsonSettingsIO (not via SettingsList)
        // to allow proper clamping against READER_ACTION_COUNT.
        // Persisted last-chosen sort. Hidden from the settings UI (no category set) —
        // edited only via the in-activity sort menu. Listed here so JsonSettingsIO
        // round-trips it through settings.json. The enumValues just exist for
        // validation clamping in loadSettings; users never see them.
        SettingInfo::Enum(
            StrId::STR_SORT_BY, &CrossPointSettings::sortMode,
            {StrId::STR_SORT_ALPHA_ASC, StrId::STR_SORT_ALPHA_DESC, StrId::STR_SORT_AUTHOR_ASC,
             StrId::STR_SORT_AUTHOR_DESC, StrId::STR_SORT_LAST_OPENED_NEW, StrId::STR_SORT_LAST_OPENED_OLD,
             StrId::STR_SORT_PROGRESS_MOST, StrId::STR_SORT_PROGRESS_LEAST, StrId::STR_SORT_DATE_ADDED_NEW,
             StrId::STR_SORT_DATE_ADDED_OLD, StrId::STR_SORT_BOOKFUSION_FIRST, StrId::STR_SORT_BOOKFUSION_LAST,
             StrId::STR_SORT_TAG_ASC, StrId::STR_SORT_TAG_DESC},
            "sortMode"),

        // --- System ---
        SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                          {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                          "sleepTimeout", StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                            StrId::STR_CAT_SYSTEM),
        SettingInfo::Toggle(StrId::STR_DEV_MODE, &CrossPointSettings::devMode, "devMode", StrId::STR_CAT_SYSTEM),
        // --- Stats tab ---
        SettingInfo::Enum(
            StrId::STR_DAILY_GOAL, &CrossPointSettings::dailyReadingGoal,
            {StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30, StrId::STR_MIN_60},
            "dailyReadingGoal", StrId::STR_CAT_STATS),
        SettingInfo::Enum(StrId::STR_MIN_SESSION_THRESHOLD, &CrossPointSettings::minSessionMinutes,
                          {StrId::STR_MIN_1, StrId::STR_MIN_3, StrId::STR_MIN_5}, "minSessionMinutes",
                          StrId::STR_CAT_STATS),
        // Persisted time-zone preset (index into TimeZoneRegistry). Hidden from the
        // device tab UI (no category) — selection happens via the Time Zone Action
        // entry, which launches TimeZoneSelectActivity. Listed here so JsonSettingsIO
        // round-trips it through settings.json.
        SettingInfo::Value(StrId::STR_TIME_ZONE, &CrossPointSettings::timeZonePreset, {0, 28, 1}, "timeZonePreset"),
        // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
              KOREADER_STORE.saveToFile();
            },
            "koUsername", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
            [](const std::string& v) {
              KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
              KOREADER_STORE.saveToFile();
            },
            "koPassword", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicString(
            StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
            [](const std::string& v) {
              KOREADER_STORE.setServerUrl(v);
              KOREADER_STORE.saveToFile();
            },
            "koServerUrl", StrId::STR_KOREADER_SYNC),
        SettingInfo::DynamicEnum(
            StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
            [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
            [](uint8_t v) {
              KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
              KOREADER_STORE.saveToFile();
            },
            "koMatchMethod", StrId::STR_KOREADER_SYNC),

        // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
        SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                            "opdsServerUrl", StrId::STR_OPDS_BROWSER),
        SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                            StrId::STR_OPDS_BROWSER),
        SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                            StrId::STR_OPDS_BROWSER)
            .withObfuscated(),
        // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
        SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                            "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage,
                            "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR, &CrossPointSettings::statusBarProgressBar,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarProgressBar",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarProgressBarThickness,
                          {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
                          "statusBarProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                          {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_TIME_LEFT, &CrossPointSettings::statusBarTimeLeft,
                          {StrId::STR_HIDE, StrId::STR_CHAPTER, StrId::STR_BOOK}, "statusBarTimeLeft",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Clock entries (web settings only; device UI uses ClockOffsetActivity for the offset).
        // Range 0..104 = quarter-hour steps from UTC-12:00 to UTC+14:00, biased by 48.
        SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Value(StrId::STR_CLOCK_UTC_OFFSET, &CrossPointSettings::clockUtcOffsetQ, {0, 104, 1},
                           "clockUtcOffsetQ", StrId::STR_CUSTOMISE_STATUS_BAR),
        SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat,
                          {StrId::STR_CLOCK_FORMAT_24H, StrId::STR_CLOCK_FORMAT_12H}, "clockFormat",
                          StrId::STR_CUSTOMISE_STATUS_BAR),
        // Persistence flag for NTP debounce. Resetting from the web UI forces a re-sync
        // on next WiFi connect, which is useful when crossing time zones.
        SettingInfo::Toggle(StrId::STR_CLOCK_SYNCED, &CrossPointSettings::clockHasBeenSynced, "clockHasBeenSynced",
                            StrId::STR_CUSTOMISE_STATUS_BAR),
    };
    // Only show tilt page turn setting when the QMI8658 IMU is present (X3)
    if (halTiltSensor.isAvailable()) {
      // Insert after the short power button setting (end of Controls section)
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->nameId == StrId::STR_SHORT_PWR_BTN) {
          v.insert(it + 1, SettingInfo::Enum(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn,
                                             {StrId::STR_STATE_OFF, StrId::STR_NORMAL, StrId::STR_INVERTED},
                                             "tiltPageTurn", StrId::STR_CAT_SYSTEM));
          break;
        }
      }
    }
    return v;
  }();
  return list;
}
