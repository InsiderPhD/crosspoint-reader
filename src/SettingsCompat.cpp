#include "SettingsCompat.h"

#include <cstdint>
#include <cstring>

// The KatiePoint numbering below is spelled out rather than taken from
// CrossPointSettings.h so this file builds on the host without the HAL. The
// static_asserts at the bottom tie every value to the real enums on device
// builds, so a renumbering there fails the build instead of corrupting imports.
//
// Upstream numbering is from CrossPoint 2.0.0 (src/CrossPointSettings.h,
// src/SettingsList.cpp at tag 2.0.0).

namespace {

// ---- KatiePoint values ----
namespace kp {
constexpr uint8_t SEAMLESS_NEVER = 0, SEAMLESS_AFTER_TIMEOUT = 1, SEAMLESS_ALWAYS = 2;
constexpr uint8_t REFRESH_30 = 4, REFRESH_COUNT = 5;
constexpr uint8_t THEME_LYRA_3_COVERS = 2, THEME_LYRA_LIBRARY = 3;
constexpr uint8_t FONT_OPENDYSLEXIC = 2;
constexpr uint8_t FONT_SIZE_COUNT = 4;
// lineHeight: the same 7-step percentage table and numbering as upstream (default +20%).
constexpr uint8_t LINE_HEIGHT_DEFAULT = 2, LINE_HEIGHT_COUNT = 7;
constexpr uint8_t LEGACY_LINE_SPACING_COUNT = 3;  // pre-table "lineSpacing": Tight / Normal / Wide
constexpr uint8_t CHAR_SPACING_COUNT = 7;
constexpr uint8_t INDENT_BOOK = 0, INDENT_NONE = 1, INDENT_COUNT = 6;
constexpr uint8_t IMAGES_DISPLAY = 0, IMAGE_RENDERING_COUNT = 3;
constexpr uint8_t SLEEP_TIMEOUT_COUNT = 5;
constexpr uint8_t FINISHED_OFF = 0, FINISHED_READ = 1;
constexpr uint8_t POS_HIDE = 0, POS_LEFT = 1, POS_MIDDLE = 2, POS_RIGHT = 3;
constexpr uint8_t XTC_BOTTOM = 1, XTC_BOTH = 3;
constexpr uint8_t TITLE_BOOK = 0, TITLE_CHAPTER = 1, TITLE_HIDE = 2;
constexpr uint8_t ACT_NONE = 0, ACT_PAGE_FORWARD = 1, ACT_PAGE_BACK = 2, ACT_SKIP_FORWARD = 3, ACT_SKIP_BACK = 4,
                  ACT_OPEN_MENU = 5, ACT_SYNC = 9, ACT_BOOKMARK = 10, ACT_FORCE_REFRESH = 11, ACT_FOOTNOTES = 15,
                  ACT_ROTATE = 20, ACT_CLIPPING = 21, ACT_DICTIONARY = 24;
}  // namespace kp

// ---- Upstream CrossPoint 2.0 values ----
namespace up {
constexpr uint8_t SLEEP_DARK = 0, SLEEP_LIGHT = 1, SLEEP_CUSTOM = 2, SLEEP_COVER = 3, SLEEP_COVER_CUSTOM = 4,
                  SLEEP_BLANK = 5, SLEEP_QUICK_RESUME = 6, SLEEP_TRANSPARENT_CUSTOM = 7;
constexpr uint8_t COVER_MODE_CROP = 1;
constexpr uint8_t REFRESH_COUNT = 6;  // 5 = Never
constexpr uint8_t THEME_LYRA_3_COVERS = 2;
constexpr uint8_t STORED_NOTOSANS = 1, LEGACY_OPENDYSLEXIC = 2;
constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
constexpr uint8_t LINE_HEIGHT_COUNT = 7;
constexpr uint8_t CHAR_SPACING_COUNT = 5;
constexpr uint8_t INDENT_MAX = 5;
constexpr uint8_t IMAGES_DISPLAY = 0, IMAGES_DISPLAY_BW = 3, IMAGE_RENDERING_COUNT = 4;
constexpr uint8_t LEGACY_SLEEP_TIMEOUT_COUNT = 5;
constexpr uint8_t CLOCK_HIDE = 0, CLOCK_RIGHT = 1, CLOCK_LEFT = 2;
constexpr uint8_t XTC_MODE_COUNT = 3;
constexpr uint8_t SIDE_PREV_NEXT = 0, SIDE_NEXT_PREV = 1, SIDE_DISABLED = 2, SIDE_NEXT_NEXT = 3, SIDE_PREV_PREV = 4;
constexpr uint8_t LONG_OFF = 0, LONG_CHAPTER_SKIP = 1, LONG_ORIENTATION = 2;
}  // namespace up

// Upstream's minutes for KatiePoint's sleepTimeout index (1/5/10/15/30), which
// is also upstream's own pre-2.0 "sleepTimeout" numbering.
constexpr uint8_t SLEEP_TIMEOUT_MINUTES[kp::SLEEP_TIMEOUT_COUNT] = {1, 5, 10, 15, 30};

// Same meaning and numbering on both sides; copied straight through. `limit`
// is one past the largest valid value (0: any uint8, the reader clamps).
struct SameKey {
  const char* upstream;
  const char* katiepoint;
  uint8_t limit;
};
constexpr SameKey SAME_KEYS[] = {
    {"hideBatteryPercentage", "hideBatteryPercentage", 3},
    {"fadingFix", "fadingFix", 2},
    {"screenInverted", "darkMode", 2},
    {"wordSpacing", "wordSpacing", 0},
    {"screenMargin", "screenMargin", 0},
    {"paragraphAlignment", "paragraphAlignment", 5},
    {"embeddedStyle", "embeddedStyle", 2},
    {"hyphenationEnabled", "hyphenationEnabled", 2},
    {"textAntiAliasing", "textAntiAliasing", 2},
    {"extraParagraphSpacing", "extraParagraphSpacing", 2},
    {"focusReadingEnabled", "bionicReading", 2},
    {"orientation", "orientation", 4},
    {"frontButtonFollowOrientation", "frontButtonFollowOrientation", 2},
    {"frontButtonBack", "frontButtonBack", 4},
    {"frontButtonConfirm", "frontButtonConfirm", 4},
    {"frontButtonLeft", "frontButtonLeft", 4},
    {"frontButtonRight", "frontButtonRight", 4},
    {"tiltPageTurn", "tiltPageTurn", 3},
    {"showHiddenFiles", "showHiddenFiles", 2},
    {"statusBarProgressBar", "statusBarProgressBar", 3},
    {"statusBarProgressBarThickness", "statusBarProgressBarThickness", 3},
    {"clockUtcOffsetQ", "clockUtcOffsetQ", 105},
    {"clockFormat", "clockFormat", 2},
    // Both firmwares index the same 104-zone table (src/util/Timezones.cpp
    // upstream, src/util/TimeZoneRegistry.cpp here; checked identical), 255 = unset.
    {"clockTimezone", "clockTimezone", 0},
    {"clockDst", "clockDst", 3},
    {"frontlightWarmth", "frontlightWarmth", 101},
    // Plugins master switch (upstream #3877): same key, same 0/1, same
    // "on when plugin folders exist" migration on both sides.
    {"pluginsEnabled", "pluginsEnabled", 2},
};

// Folder names, the same on both sides.
// opdsDownloadFolder: the shared OPDS download folder ("" = SD root) on both.
constexpr const char* SAME_STRING_KEYS[] = {"dictionaryName", "language", "opdsDownloadFolder"};

// Keys only upstream writes into settings.json in its own numbering: no
// KatiePoint build ever wrote one of them there (older builds kept their own
// settings in settings.json; current ones keep them in katiepoint-settings.json,
// which is never run through isUpstreamDocument()). KatiePoint's own file does
// carry "pluginsEnabled" now (same meaning as upstream's), but only settings.json
// is tested here, and exportShared() writes the first two keys plus
// pluginsEnabled into it, so its output is recognised too.
constexpr const char* UPSTREAM_ONLY_KEYS[] = {
    // Not "lineHeight": KatiePoint writes that key too since adopting upstream's table.
    "sleepTimeoutMinutes", "pluginsEnabled", "paragraphIndentSpaces", "pageTurnGesture",
    "libraryUseMetadata",  "screenInverted", "focusReadingEnabled",   "readerMenuStyle",
};

// Store theme folder <-> the upstream mode drawing the same screen. Same
// names as CrossPointSettings::legacySleepThemeFor().
struct ThemeMode {
  const char* theme;
  uint8_t upstreamMode;
};
constexpr ThemeMode THEME_MODES[] = {
    {"dark", up::SLEEP_DARK},
    {"light", up::SLEEP_LIGHT},
    {"custom", up::SLEEP_CUSTOM},
    {"cover-crop", up::SLEEP_COVER},
    {"cover-or-custom", up::SLEEP_COVER_CUSTOM},
    {"blank", up::SLEEP_BLANK},
    {"page-overlay", up::SLEEP_TRANSPARENT_CUSTOM},
};

// Upstream short-power / long-press-Confirm actions <-> reader actions.
struct ActionMap {
  uint8_t upstream;
  uint8_t action;
};
// SHORT_PWRBTN. Sleep maps to None as in CrossPointSettings::migrateReaderActions
// (a power long-press already sleeps); Confirm opens the menu in the reader.
constexpr ActionMap POWER_ACTIONS[] = {
    {0, kp::ACT_NONE},      {1, kp::ACT_NONE},      {2, kp::ACT_PAGE_FORWARD}, {3, kp::ACT_FORCE_REFRESH},
    {4, kp::ACT_FOOTNOTES}, {5, kp::ACT_OPEN_MENU}, {6, kp::ACT_CLIPPING},     {7, kp::ACT_BOOKMARK},
    {8, kp::ACT_SYNC},      {9, kp::ACT_OPEN_MENU},
};
// Export order matters: the first entry for an action wins.
constexpr ActionMap POWER_EXPORT[] = {
    {0, kp::ACT_NONE},     {2, kp::ACT_PAGE_FORWARD}, {3, kp::ACT_FORCE_REFRESH}, {4, kp::ACT_FOOTNOTES},
    {6, kp::ACT_CLIPPING}, {7, kp::ACT_BOOKMARK},     {8, kp::ACT_SYNC},          {9, kp::ACT_OPEN_MENU},
};
// LONG_PRESS_MENU_FUNCTION
constexpr ActionMap CONFIRM_LONG_ACTIONS[] = {
    {0, kp::ACT_SYNC},      {1, kp::ACT_NONE},     {2, kp::ACT_BOOKMARK},  {3, kp::ACT_DICTIONARY},
    {4, kp::ACT_OPEN_MENU}, {5, kp::ACT_CLIPPING}, {6, kp::ACT_FOOTNOTES},
};

bool readU8(JsonVariantConst doc, const char* key, uint8_t& out) {
  const JsonVariantConst v = doc[key];
  if (!v.is<int>()) return false;
  const int n = v.as<int>();
  if (n < 0 || n > 255) return false;
  out = static_cast<uint8_t>(n);
  return true;
}

const char* readString(JsonVariantConst doc, const char* key) {
  const JsonVariantConst v = doc[key];
  return v.is<const char*>() ? v.as<const char*>() : nullptr;
}

bool toAction(const ActionMap* table, const size_t count, const uint8_t upstream, uint8_t& action) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].upstream == upstream) {
      action = table[i].action;
      return true;
    }
  }
  return false;
}

bool fromAction(const ActionMap* table, const size_t count, const uint8_t action, uint8_t& upstream) {
  for (size_t i = 0; i < count; i++) {
    if (table[i].action == action) {
      upstream = table[i].upstream;
      return true;
    }
  }
  return false;
}

// Side-button page direction of one button under an upstream layout.
uint8_t sideShortAction(const uint8_t layout, const bool upButton) {
  switch (layout) {
    case up::SIDE_NEXT_PREV:
      return upButton ? kp::ACT_PAGE_FORWARD : kp::ACT_PAGE_BACK;
    case up::SIDE_DISABLED:
      return kp::ACT_NONE;
    case up::SIDE_NEXT_NEXT:
      return kp::ACT_PAGE_FORWARD;
    case up::SIDE_PREV_PREV:
      return kp::ACT_PAGE_BACK;
    case up::SIDE_PREV_NEXT:
    default:
      return upButton ? kp::ACT_PAGE_BACK : kp::ACT_PAGE_FORWARD;
  }
}

uint8_t sideLongAction(const uint8_t behavior, const uint8_t shortAction) {
  switch (behavior) {
    case up::LONG_CHAPTER_SKIP:
      if (shortAction == kp::ACT_PAGE_FORWARD) return kp::ACT_SKIP_FORWARD;
      if (shortAction == kp::ACT_PAGE_BACK) return kp::ACT_SKIP_BACK;
      return kp::ACT_NONE;
    case up::LONG_ORIENTATION:
      return kp::ACT_ROTATE;
    case up::LONG_OFF:
    default:
      return kp::ACT_NONE;
  }
}

void importReaderActions(JsonVariantConst in, JsonDocument& out) {
  // Upstream's own defaults when a key is absent, so the user gets the
  // behaviour they had there rather than KatiePoint's.
  uint8_t layout = up::SIDE_PREV_NEXT;
  uint8_t longBehavior = up::LONG_OFF;
  uint8_t power = 0;        // IGNORE
  uint8_t confirmLong = 1;  // DISABLED
  readU8(in, "sideButtonLayout", layout);
  readU8(in, "longPressButtonBehavior", longBehavior);
  readU8(in, "shortPwrBtn", power);
  readU8(in, "longPressMenuFunction", confirmLong);

  const uint8_t upShort = sideShortAction(layout, true);
  const uint8_t downShort = sideShortAction(layout, false);
  out["readerShortPressSideUp"] = upShort;
  out["readerShortPressSideDown"] = downShort;
  out["readerLongPressSideUp"] = sideLongAction(longBehavior, upShort);
  out["readerLongPressSideDown"] = sideLongAction(longBehavior, downShort);

  uint8_t action;
  if (toAction(POWER_ACTIONS, sizeof(POWER_ACTIONS) / sizeof(POWER_ACTIONS[0]), power, action)) {
    out["readerShortPressPower"] = action;
  }
  if (toAction(CONFIRM_LONG_ACTIONS, sizeof(CONFIRM_LONG_ACTIONS) / sizeof(CONFIRM_LONG_ACTIONS[0]), confirmLong,
               action)) {
    out["readerLongPressConfirm"] = action;
  }
  out["readerActionsMigrated"] = 1;
}

void exportReaderActions(JsonVariantConst in, JsonDocument& out) {
  uint8_t upShort, downShort;
  if (readU8(in, "readerShortPressSideUp", upShort) && readU8(in, "readerShortPressSideDown", downShort)) {
    for (uint8_t layout = up::SIDE_PREV_NEXT; layout <= up::SIDE_PREV_PREV; layout++) {
      if (sideShortAction(layout, true) == upShort && sideShortAction(layout, false) == downShort) {
        out["sideButtonLayout"] = layout;
        break;
      }
    }
    uint8_t upLong, downLong;
    if (readU8(in, "readerLongPressSideUp", upLong) && readU8(in, "readerLongPressSideDown", downLong)) {
      for (uint8_t behavior = up::LONG_OFF; behavior <= up::LONG_ORIENTATION; behavior++) {
        if (sideLongAction(behavior, upShort) == upLong && sideLongAction(behavior, downShort) == downLong) {
          out["longPressButtonBehavior"] = behavior;
          break;
        }
      }
    }
  }
  uint8_t action, upstream;
  if (readU8(in, "readerShortPressPower", action) &&
      fromAction(POWER_EXPORT, sizeof(POWER_EXPORT) / sizeof(POWER_EXPORT[0]), action, upstream)) {
    out["shortPwrBtn"] = upstream;
  }
  if (readU8(in, "readerLongPressConfirm", action) &&
      fromAction(CONFIRM_LONG_ACTIONS, sizeof(CONFIRM_LONG_ACTIONS) / sizeof(CONFIRM_LONG_ACTIONS[0]), action,
                 upstream)) {
    out["longPressMenuFunction"] = upstream;
  }
}

void importStatusBar(JsonVariantConst in, JsonDocument& out) {
  // Upstream's reader bar is toggles on fixed spots; place each element where
  // KatiePoint's own legacy migration (migrateStatusBarPositions) would.
  uint8_t v;
  if (readU8(in, "statusBarBattery", v)) out["statusBarBatteryPos"] = v ? kp::POS_LEFT : kp::POS_HIDE;
  if (readU8(in, "statusBarTitle", v)) {
    out["statusBarBookTitlePos"] = v == kp::TITLE_BOOK ? kp::POS_MIDDLE : kp::POS_HIDE;
    out["statusBarChapterTitlePos"] = v == kp::TITLE_CHAPTER ? kp::POS_MIDDLE : kp::POS_HIDE;
  }
  if (readU8(in, "statusBarBookProgressPercentage", v)) {
    out["statusBarBookPercentPos"] = v ? kp::POS_RIGHT : kp::POS_HIDE;
  }
  if (readU8(in, "statusBarChapterPageCount", v)) {
    out["statusBarChapterPagePos"] = v ? kp::POS_RIGHT : kp::POS_HIDE;
    // Its presence also tells loadSettings() this is not a pre-refactor file.
    out["statusBarChapterPageCount"] = v ? 1 : 0;
  }
  if (readU8(in, "statusBarClock", v)) {
    out["statusBarClockPos"] = v == up::CLOCK_RIGHT ? kp::POS_RIGHT : v == up::CLOCK_LEFT ? kp::POS_LEFT : kp::POS_HIDE;
  }
  if (readU8(in, "xtcStatusBarMode", v) && v < up::XTC_MODE_COUNT) out["xtcStatusBarMode"] = v;
  // The Outside Reader bar is derived from these two by migrateHeaderStatusBar().
  if (readU8(in, "clockShowHeader", v)) out["showClockOutsideReader"] = v ? 1 : 0;
}

void exportStatusBar(JsonVariantConst in, JsonDocument& out) {
  uint8_t v;
  if (readU8(in, "statusBarBatteryPos", v)) out["statusBarBattery"] = v != kp::POS_HIDE ? 1 : 0;
  uint8_t book, chapter;
  if (readU8(in, "statusBarBookTitlePos", book) && readU8(in, "statusBarChapterTitlePos", chapter)) {
    out["statusBarTitle"] = chapter != kp::POS_HIDE ? kp::TITLE_CHAPTER
                            : book != kp::POS_HIDE  ? kp::TITLE_BOOK
                                                    : kp::TITLE_HIDE;
  }
  if (readU8(in, "statusBarBookPercentPos", v)) out["statusBarBookProgressPercentage"] = v != kp::POS_HIDE ? 1 : 0;
  if (readU8(in, "statusBarChapterPagePos", v)) out["statusBarChapterPageCount"] = v != kp::POS_HIDE ? 1 : 0;
  if (readU8(in, "statusBarClockPos", v)) {
    out["statusBarClock"] = v == kp::POS_HIDE ? up::CLOCK_HIDE : v == kp::POS_LEFT ? up::CLOCK_LEFT : up::CLOCK_RIGHT;
  }
  if (readU8(in, "xtcStatusBarMode", v)) out["xtcStatusBarMode"] = v == kp::XTC_BOTH ? kp::XTC_BOTTOM : v;
  if (readU8(in, "showClockOutsideReader", v)) out["clockShowHeader"] = v ? 1 : 0;
}

uint8_t nearestSleepTimeoutIndex(const uint8_t minutes) {
  uint8_t best = 0;
  int bestDiff = 256;
  for (uint8_t i = 0; i < kp::SLEEP_TIMEOUT_COUNT; i++) {
    const int diff =
        minutes > SLEEP_TIMEOUT_MINUTES[i] ? minutes - SLEEP_TIMEOUT_MINUTES[i] : SLEEP_TIMEOUT_MINUTES[i] - minutes;
    if (diff < bestDiff) {
      best = i;
      bestDiff = diff;
    }
  }
  return best;
}

}  // namespace

bool SettingsCompat::isUpstreamDocument(JsonVariantConst doc) {
  for (const char* key : UPSTREAM_ONLY_KEYS) {
    if (!doc[key].isNull()) return true;
  }
  return false;
}

void SettingsCompat::importUpstream(JsonVariantConst in, JsonDocument& out) {
  uint8_t v;
  for (const auto& k : SAME_KEYS) {
    if (readU8(in, k.upstream, v) && (k.limit == 0 || v < k.limit)) out[k.katiepoint] = v;
  }
  for (const char* key : SAME_STRING_KEYS) {
    if (const char* s = readString(in, key)) out[key] = s;
  }

  // Sleep screen: the store theme that draws the same screen (the names
  // CrossPointSettings::legacySleepThemeFor() gives the retired modes). Quick
  // Resume is KatiePoint's "Seamless: Always"; Cover + Transparent has no
  // counterpart (theme default).
  uint8_t sleepMode = up::SLEEP_DARK;
  const bool hasSleepMode = readU8(in, "sleepScreen", sleepMode);
  if (hasSleepMode) {
    for (const auto& tm : THEME_MODES) {
      if (tm.upstreamMode == sleepMode) {
        out["sleepTheme"] = tm.theme;
        break;
      }
    }
  }
  if (hasSleepMode && sleepMode == up::SLEEP_QUICK_RESUME) {
    out["seamlessSleepScreen"] = kp::SEAMLESS_ALWAYS;
  } else if (readU8(in, "quickResumeSleepScreen", v)) {
    out["seamlessSleepScreen"] = v ? kp::SEAMLESS_AFTER_TIMEOUT : kp::SEAMLESS_NEVER;
  }

  // "Never" has no KatiePoint counterpart; the longest interval is closest.
  if (readU8(in, "refreshFrequency", v) && v < up::REFRESH_COUNT) {
    out["refreshFrequency"] = v < kp::REFRESH_COUNT ? v : kp::REFRESH_30;
  }
  // Classic / Lyra / Lyra 3 Covers share numbers; RoundedRaff and Cover Grid
  // do not exist here (4 is Carousel), so those keep the default.
  if (readU8(in, "uiTheme", v) && v <= up::THEME_LYRA_3_COVERS) out["uiTheme"] = v;

  // Fonts: upstream's only built-in is Noto Sans; everything else is an SD
  // family by name. Its legacy OpenDyslexic id is our built-in OpenDyslexic.
  const char* sdFont = readString(in, "sdFontFamilyName");
  if (sdFont && sdFont[0] != '\0') {
    out["sdFontFamilyName"] = sdFont;
  } else if (readU8(in, "fontFamily", v) && v == up::LEGACY_OPENDYSLEXIC) {
    out["fontFamily"] = kp::FONT_OPENDYSLEXIC;
  }
  // Point size (legacy 0..3 = the same Small..X-Large slots we use). The
  // slots were 12/14/16/18 pt upstream; snap anything else to the nearest.
  if (readU8(in, "fontSize", v)) {
    if (v <= up::LEGACY_FONT_SIZE_MAX) {
      out["fontSize"] = v;
    } else {
      const int slot = v <= 12 ? 0 : (v - 12 + 1) / 2;
      out["fontSize"] = static_cast<uint8_t>(slot < kp::FONT_SIZE_COUNT ? slot : kp::FONT_SIZE_COUNT - 1);
    }
  }
  // Same table both sides. Pre-lineHeight upstream (Tight/Normal/Wide/Extra Wide, 0..3)
  // moves up one slot, as upstream's own migration does.
  if (readU8(in, "lineHeight", v) && v < up::LINE_HEIGHT_COUNT) {
    out["lineHeight"] = v;
  } else if (readU8(in, "lineSpacing", v)) {
    out["lineHeight"] = v + 1 < up::LINE_HEIGHT_COUNT ? static_cast<uint8_t>(v + 1) : kp::LINE_HEIGHT_DEFAULT;
  }
  // Upstream: -2..+2 whole px (2 = none). Here: -1.5..+1.5 half px (3 = none).
  if (readU8(in, "characterSpacing", v) && v < up::CHAR_SPACING_COUNT) {
    static constexpr uint8_t TO_KP[up::CHAR_SPACING_COUNT] = {0, 1, 3, 5, 6};
    out["characterSpacing"] = TO_KP[v];
  }
  // Upstream: 0 = none, 1..5 spaces. Here: 0 = the book's, 1 = none, 2..5 = 1..4.
  if (readU8(in, "paragraphIndentSpaces", v) && v <= up::INDENT_MAX) {
    const uint8_t indent = static_cast<uint8_t>(v + kp::INDENT_NONE);
    out["paragraphIndent"] = indent < kp::INDENT_COUNT ? indent : kp::INDENT_COUNT - 1;
  }
  // Display (B/W) is our Display with Image Grayscale off.
  if (readU8(in, "imageRendering", v) && v < up::IMAGE_RENDERING_COUNT) {
    if (v == up::IMAGES_DISPLAY_BW) {
      out["imageRendering"] = kp::IMAGES_DISPLAY;
      out["imageGrayscale"] = 0;
    } else {
      out["imageRendering"] = v;
      if (v == up::IMAGES_DISPLAY) out["imageGrayscale"] = 1;
    }
  }
  // Minutes (31 = never) to the nearest of 1/5/10/15/30.
  if (readU8(in, "sleepTimeoutMinutes", v)) {
    out["sleepTimeout"] = nearestSleepTimeoutIndex(v);
  } else if (readU8(in, "sleepTimeout", v) && v < up::LEGACY_SLEEP_TIMEOUT_COUNT) {
    out["sleepTimeout"] = v;
  }
  if (readU8(in, "moveFinishedToReadFolder", v)) out["finishedBookFolder"] = v ? kp::FINISHED_READ : kp::FINISHED_OFF;

  // Frontlight: upstream keeps brightness and on/off apart; here 0 is off and
  // the last level is what the toggle restores.
  uint8_t brightness;
  if (readU8(in, "frontlightBrightness", brightness) && brightness <= 100) {
    uint8_t on = 1;
    readU8(in, "frontlightOn", on);
    out["frontlightBrightness"] = on ? brightness : 0;
    if (brightness > 0) out["frontlightLastBrightness"] = brightness;
  }

  importStatusBar(in, out);
  importReaderActions(in, out);
}

void SettingsCompat::exportShared(JsonVariantConst in, JsonDocument& out) {
  uint8_t v;
  for (const auto& k : SAME_KEYS) {
    if (readU8(in, k.katiepoint, v) && (k.limit == 0 || v < k.limit)) out[k.upstream] = v;
  }
  for (const char* key : SAME_STRING_KEYS) {
    if (const char* s = readString(in, key)) out[key] = s;
  }

  // Sleep screen. A theme with no upstream mode (a downloaded one) leaves
  // upstream's choice alone.
  if (const char* theme = readString(in, "sleepTheme")) {
    for (const auto& tm : THEME_MODES) {
      if (strcmp(theme, tm.theme) == 0) {
        out["sleepScreen"] = tm.upstreamMode;
        if (tm.upstreamMode == up::SLEEP_COVER) out["sleepScreenCoverMode"] = up::COVER_MODE_CROP;
        break;
      }
    }
  }
  if (readU8(in, "seamlessSleepScreen", v)) {
    if (v == kp::SEAMLESS_ALWAYS) out["sleepScreen"] = up::SLEEP_QUICK_RESUME;
    out["quickResumeSleepScreen"] = v == kp::SEAMLESS_AFTER_TIMEOUT ? 1 : 0;
  }

  if (readU8(in, "refreshFrequency", v) && v < kp::REFRESH_COUNT) out["refreshFrequency"] = v;
  if (readU8(in, "uiTheme", v)) {
    if (v <= kp::THEME_LYRA_3_COVERS) {
      out["uiTheme"] = v;
    } else if (v == kp::THEME_LYRA_LIBRARY) {  // retired; loads as Lyra 3 Covers
      out["uiTheme"] = up::THEME_LYRA_3_COVERS;
    }
  }

  // Fonts: an SD family by name; our built-in OpenDyslexic as upstream's legacy
  // id (its loader turns that into its SD OpenDyslexic, or Noto Sans when that
  // is not installed); any other built-in as Noto Sans, the only one there.
  const char* sdFont = readString(in, "sdFontFamilyName");
  out["sdFontFamilyName"] = sdFont ? sdFont : "";
  out["fontFamily"] = (!sdFont || sdFont[0] == '\0') && readU8(in, "fontFamily", v) && v == kp::FONT_OPENDYSLEXIC
                          ? up::LEGACY_OPENDYSLEXIC
                          : up::STORED_NOTOSANS;
  if (readU8(in, "fontSize", v) && v < kp::FONT_SIZE_COUNT) out["fontSize"] = static_cast<uint8_t>(12 + v * 2);
  // Always written, like sleepTimeoutMinutes below (which marks the file as
  // upstream-numbered for isUpstreamDocument()). Same table both sides; an older
  // KatiePoint document's "lineSpacing" (Tight / Normal / Wide) moves up one slot.
  uint8_t lineHeight = kp::LINE_HEIGHT_DEFAULT;
  if (readU8(in, "lineHeight", v) && v < kp::LINE_HEIGHT_COUNT) {
    lineHeight = v;
  } else if (readU8(in, "lineSpacing", v) && v < kp::LEGACY_LINE_SPACING_COUNT) {
    lineHeight = static_cast<uint8_t>(v + 1);
  }
  out["lineHeight"] = lineHeight;
  if (readU8(in, "characterSpacing", v) && v < kp::CHAR_SPACING_COUNT) {
    // Nearest whole pixel, half pixels away from zero, so every upstream value
    // survives a round trip through importUpstream().
    static constexpr uint8_t TO_UP[kp::CHAR_SPACING_COUNT] = {0, 1, 1, 2, 3, 3, 4};
    out["characterSpacing"] = TO_UP[v];
  }
  // "The book's own indent" has no upstream setting; leave upstream's alone.
  if (readU8(in, "paragraphIndent", v) && v != kp::INDENT_BOOK && v < kp::INDENT_COUNT) {
    out["paragraphIndentSpaces"] = static_cast<uint8_t>(v - kp::INDENT_NONE);
  }
  if (readU8(in, "imageRendering", v) && v < kp::IMAGE_RENDERING_COUNT) {
    uint8_t grayscale = 1;
    readU8(in, "imageGrayscale", grayscale);
    out["imageRendering"] = v == kp::IMAGES_DISPLAY && !grayscale ? up::IMAGES_DISPLAY_BW : v;
  }
  uint8_t sleepTimeout = 2;  // 10 min, the default on both sides
  if (readU8(in, "sleepTimeout", v) && v < kp::SLEEP_TIMEOUT_COUNT) sleepTimeout = v;
  out["sleepTimeoutMinutes"] = SLEEP_TIMEOUT_MINUTES[sleepTimeout];
  // /Completed has no upstream equivalent; leave upstream's choice alone.
  if (readU8(in, "finishedBookFolder", v) && (v == kp::FINISHED_OFF || v == kp::FINISHED_READ)) {
    out["moveFinishedToReadFolder"] = v == kp::FINISHED_READ ? 1 : 0;
  }

  // Frontlight (frontlight builds only carry frontlightBrightness).
  uint8_t brightness;
  if (readU8(in, "frontlightBrightness", brightness) && brightness <= 100) {
    out["frontlightOn"] = brightness > 0 ? 1 : 0;
    uint8_t last = 0;
    if (brightness > 0) {
      out["frontlightBrightness"] = brightness;
    } else if (readU8(in, "frontlightLastBrightness", last) && last > 0 && last <= 100) {
      out["frontlightBrightness"] = last;
    }
  }

  exportStatusBar(in, out);
  exportReaderActions(in, out);
}

#ifndef SETTINGS_COMPAT_HOST_TEST
#include "CrossPointSettings.h"
#include "util/TimeZoneRegistry.h"

namespace {
using S = CrossPointSettings;
static_assert(kp::SEAMLESS_NEVER == S::SEAMLESS_NEVER && kp::SEAMLESS_AFTER_TIMEOUT == S::SEAMLESS_AFTER_TIMEOUT &&
              kp::SEAMLESS_ALWAYS == S::SEAMLESS_ALWAYS);
static_assert(kp::REFRESH_30 == S::REFRESH_30 && kp::REFRESH_COUNT == S::REFRESH_FREQUENCY_COUNT);
static_assert(kp::THEME_LYRA_3_COVERS == S::LYRA_3_COVERS && kp::THEME_LYRA_LIBRARY == S::LYRA_LIBRARY &&
              S::CLASSIC == 0 && S::LYRA == 1);
static_assert(kp::FONT_OPENDYSLEXIC == S::OPENDYSLEXIC && kp::FONT_SIZE_COUNT == S::FONT_SIZE_COUNT);
static_assert(kp::LINE_HEIGHT_DEFAULT == S::LINE_SPACING_120 && kp::LINE_HEIGHT_COUNT == S::LINE_COMPRESSION_COUNT &&
              kp::LINE_HEIGHT_COUNT == up::LINE_HEIGHT_COUNT &&
              kp::LEGACY_LINE_SPACING_COUNT == S::LEGACY_LINE_SPACING_COUNT);
static_assert(kp::CHAR_SPACING_COUNT == S::CHARACTER_SPACING_COUNT && S::CHARACTER_SPACING_ZERO == 3);
static_assert(kp::INDENT_BOOK == TextLayoutOptions::PARAGRAPH_INDENT_BOOK &&
              kp::INDENT_NONE == TextLayoutOptions::PARAGRAPH_INDENT_NONE &&
              kp::INDENT_COUNT == TextLayoutOptions::PARAGRAPH_INDENT_COUNT);
static_assert(kp::IMAGES_DISPLAY == S::IMAGES_DISPLAY && kp::IMAGE_RENDERING_COUNT == S::IMAGE_RENDERING_COUNT);
static_assert(kp::SLEEP_TIMEOUT_COUNT == S::SLEEP_TIMEOUT_COUNT && S::SLEEP_1_MIN == 0 && S::SLEEP_5_MIN == 1 &&
              S::SLEEP_10_MIN == 2 && S::SLEEP_15_MIN == 3 && S::SLEEP_30_MIN == 4);
static_assert(kp::FINISHED_OFF == S::FINISHED_FOLDER_OFF && kp::FINISHED_READ == S::FINISHED_FOLDER_READ);
static_assert(kp::POS_HIDE == S::SB_POS_HIDE && kp::POS_LEFT == S::SB_POS_LEFT && kp::POS_MIDDLE == S::SB_POS_MIDDLE &&
              kp::POS_RIGHT == S::SB_POS_RIGHT);
static_assert(kp::XTC_BOTTOM == S::XTC_STATUS_BAR_BOTTOM && kp::XTC_BOTH == S::XTC_STATUS_BAR_BOTH);
static_assert(kp::TITLE_BOOK == S::BOOK_TITLE && kp::TITLE_CHAPTER == S::CHAPTER_TITLE &&
              kp::TITLE_HIDE == S::HIDE_TITLE);
static_assert(S::HIDE_BATTERY_PERCENTAGE_COUNT == 3 && S::PARAGRAPH_ALIGNMENT_COUNT == 5 && S::ORIENTATION_COUNT == 4 &&
              S::TILT_PAGE_TURN_COUNT == 3 && S::FRONT_BUTTON_HARDWARE_COUNT == 4 &&
              S::STATUS_BAR_PROGRESS_BAR_COUNT == 3 && S::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT == 3);
static_assert(TimeZoneRegistry::DST_AUTO == 0 && TimeZoneRegistry::DST_ON == 1 && TimeZoneRegistry::DST_OFF == 2 &&
              TimeZoneRegistry::NOT_CHOSEN == 255);
static_assert(kp::ACT_NONE == S::READER_ACTION_NONE && kp::ACT_PAGE_FORWARD == S::READER_ACTION_PAGE_FORWARD &&
              kp::ACT_PAGE_BACK == S::READER_ACTION_PAGE_BACK &&
              kp::ACT_SKIP_FORWARD == S::READER_ACTION_SKIP_CHAPTER_FORWARD &&
              kp::ACT_SKIP_BACK == S::READER_ACTION_SKIP_CHAPTER_BACK &&
              kp::ACT_OPEN_MENU == S::READER_ACTION_OPEN_MENU && kp::ACT_SYNC == S::READER_ACTION_SYNC &&
              kp::ACT_BOOKMARK == S::READER_ACTION_BOOKMARK &&
              kp::ACT_FORCE_REFRESH == S::READER_ACTION_FORCE_REFRESH &&
              kp::ACT_FOOTNOTES == S::READER_ACTION_FOOTNOTES && kp::ACT_ROTATE == S::READER_ACTION_ROTATE_SCREEN &&
              kp::ACT_CLIPPING == S::READER_ACTION_CREATE_CLIPPING &&
              kp::ACT_DICTIONARY == S::READER_ACTION_DICTIONARY);
}  // namespace
#endif
