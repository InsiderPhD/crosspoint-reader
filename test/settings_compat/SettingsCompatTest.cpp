// Host test for src/SettingsCompat.cpp: the KatiePoint <-> upstream CrossPoint
// 2.0 settings.json translation, both directions. Run test/run_settings_compat_test.sh.
#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "SettingsCompat.h"

namespace {
int failures = 0;

void expectInt(JsonVariantConst doc, const char* key, int want, const char* label) {
  if (!doc[key].is<int>() || doc[key].as<int>() != want) {
    std::printf("FAIL %s: %s = %s, want %d\n", label, key,
                doc[key].isNull() ? "(absent)" : std::to_string(doc[key].as<int>()).c_str(), want);
    failures++;
  }
}

void expectAbsent(JsonVariantConst doc, const char* key, const char* label) {
  if (!doc[key].isNull()) {
    std::printf("FAIL %s: %s should be absent\n", label, key);
    failures++;
  }
}

void expectStr(JsonVariantConst doc, const char* key, const char* want, const char* label) {
  const char* got = doc[key].as<const char*>();
  if (!got || std::strcmp(got, want) != 0) {
    std::printf("FAIL %s: %s = %s, want %s\n", label, key, got ? got : "(absent)", want);
    failures++;
  }
}

void expectTrue(bool v, const char* label) {
  if (!v) {
    std::printf("FAIL %s\n", label);
    failures++;
  }
}

void parse(JsonDocument& doc, const char* json) {
  if (deserializeJson(doc, json)) {
    std::printf("FAIL bad test JSON\n");
    failures++;
  }
}

// Trimmed from a settings.json CrossPoint 2.0.0 writes on an X4.
constexpr const char* UPSTREAM = R"({
  "sleepScreen": 4, "sleepScreenCoverMode": 0, "sleepScreenCoverFilter": 0, "quickResumeSleepScreen": 1,
  "hideBatteryPercentage": 1, "refreshFrequency": 5, "uiTheme": 4, "fadingFix": 1, "screenInverted": 1,
  "fontFamily": 1, "fontSize": 16, "lineHeight": 3, "wordSpacing": 125, "characterSpacing": 3,
  "screenMargin": 15, "paragraphAlignment": 1, "embeddedStyle": 0, "focusReadingEnabled": 1,
  "hyphenationEnabled": 1, "orientation": 2, "extraParagraphSpacing": 0, "paragraphIndentSpaces": 3,
  "textAntiAliasing": 0, "imageRendering": 3, "readerMenuStyle": 0, "sideButtonLayout": 1,
  "frontButtonFollowOrientation": 1, "longPressButtonBehavior": 1, "longPressMenuFunction": 2,
  "shortPwrBtn": 2, "tiltPageTurn": 1, "pwrBtnFootnoteBack": 1, "backShortToFileBrowser": 0,
  "pluginsEnabled": 0, "pluginHubPromptHidden": 0, "sleepTimeoutMinutes": 12, "showHiddenFiles": 1,
  "libraryUseMetadata": 1, "removeReadBooksFromRecents": 0, "moveFinishedToReadFolder": 1,
  "opdsDownloadFolder": "/Books/OPDS", "opdsFilenameFormat": 0, "frontlightBrightness": 40, "frontlightOn": 0,
  "statusBarChapterPageCount": 0, "statusBarBookProgressPercentage": 1, "statusBarProgressBar": 0,
  "statusBarProgressBarThickness": 2, "statusBarTitle": 0, "statusBarBattery": 1, "xtcStatusBarMode": 2,
  "statusBarClock": 2, "clockUtcOffsetQ": 52, "clockFormat": 1, "clockTimezone": 37, "clockDst": 2,
  "clockShowHeader": 1, "clockHasBeenSynced": 1, "frontButtonBack": 1, "frontButtonConfirm": 0,
  "frontButtonLeft": 2, "frontButtonRight": 3, "sdFontFamilyName": "Literata", "dictionaryName": "en-en",
  "language": "DE"
})";

void testDetection() {
  JsonDocument up;
  parse(up, UPSTREAM);
  expectTrue(SettingsCompat::isUpstreamDocument(up.as<JsonVariantConst>()), "upstream file detected");

  // An older KatiePoint settings.json: none of the upstream-only keys.
  JsonDocument kp;
  parse(kp, R"({"sleepScreen":3,"fontSize":1,"lineSpacing":1,"sleepTimeout":2,"shortPwrBtn":0,
                "readerActionsMigrated":1,"statusBarBatteryPos":1,"sleepTheme":"light"})");
  expectTrue(!SettingsCompat::isUpstreamDocument(kp.as<JsonVariantConst>()), "KatiePoint file not upstream");

  // Our own export must read back as upstream-numbered.
  JsonDocument out;
  SettingsCompat::exportShared(kp.as<JsonVariantConst>(), out);
  expectTrue(SettingsCompat::isUpstreamDocument(out.as<JsonVariantConst>()), "export output detected as upstream");
}

void testImport() {
  JsonDocument up;
  parse(up, UPSTREAM);
  JsonDocument kp;
  SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
  const JsonVariantConst k = kp.as<JsonVariantConst>();
  const char* L = "import";

  expectStr(k, "sleepTheme", "cover-or-custom", L);  // UP Cover+Custom (4)
  expectAbsent(k, "sleepScreen", L);
  expectInt(k, "seamlessSleepScreen", 1, L);  // quick resume after timeout
  expectInt(k, "hideBatteryPercentage", 1, L);
  expectInt(k, "refreshFrequency", 4, L);  // Never -> 30 pages
  expectAbsent(k, "uiTheme", L);           // Cover Grid: no counterpart
  expectInt(k, "fadingFix", 1, L);
  expectInt(k, "darkMode", 1, L);
  expectStr(k, "sdFontFamilyName", "Literata", L);
  expectAbsent(k, "fontFamily", L);
  expectInt(k, "fontSize", 2, L);     // 16pt -> Large
  expectInt(k, "lineSpacing", 2, L);  // lineHeight 3 (+30%) -> Wide
  expectInt(k, "wordSpacing", 125, L);
  expectInt(k, "characterSpacing", 5, L);  // +1px -> +1.0 (index 5)
  expectInt(k, "screenMargin", 15, L);
  expectInt(k, "paragraphAlignment", 1, L);
  expectInt(k, "embeddedStyle", 0, L);
  expectInt(k, "bionicReading", 1, L);
  expectInt(k, "hyphenationEnabled", 1, L);
  expectInt(k, "orientation", 2, L);
  expectInt(k, "extraParagraphSpacing", 0, L);
  expectInt(k, "paragraphIndent", 4, L);  // 3 spaces
  expectInt(k, "textAntiAliasing", 0, L);
  expectInt(k, "imageRendering", 0, L);  // Display (B/W) -> Display + grayscale off
  expectInt(k, "imageGrayscale", 0, L);
  expectInt(k, "frontButtonFollowOrientation", 1, L);
  expectInt(k, "tiltPageTurn", 1, L);
  expectInt(k, "sleepTimeout", 2, L);  // 12 min -> 10
  expectInt(k, "showHiddenFiles", 1, L);
  expectInt(k, "finishedBookFolder", 1, L);
  expectInt(k, "frontlightBrightness", 0, L);  // light off
  expectInt(k, "frontlightLastBrightness", 40, L);
  expectInt(k, "statusBarChapterPagePos", 0, L);
  expectInt(k, "statusBarChapterPageCount", 0, L);
  expectInt(k, "statusBarBookPercentPos", 3, L);
  expectInt(k, "statusBarProgressBar", 0, L);
  expectInt(k, "statusBarProgressBarThickness", 2, L);
  expectInt(k, "statusBarBookTitlePos", 2, L);
  expectInt(k, "statusBarChapterTitlePos", 0, L);
  expectInt(k, "statusBarBatteryPos", 1, L);
  expectInt(k, "xtcStatusBarMode", 2, L);
  expectInt(k, "statusBarClockPos", 1, L);  // upstream Left
  expectInt(k, "showClockOutsideReader", 1, L);
  expectInt(k, "clockUtcOffsetQ", 52, L);
  expectInt(k, "clockFormat", 1, L);
  expectInt(k, "clockTimezone", 37, L);
  expectInt(k, "clockDst", 2, L);
  expectInt(k, "frontButtonBack", 1, L);
  expectInt(k, "frontButtonConfirm", 0, L);
  expectStr(k, "dictionaryName", "en-en", L);
  expectStr(k, "language", "DE", L);
  expectStr(k, "opdsDownloadFolder", "/Books/OPDS", L);
  // Reader actions: side buttons Next/Prev with long-press chapter skip,
  // power = page turn, long-press Confirm = bookmark.
  expectInt(k, "readerShortPressSideUp", 1, L);
  expectInt(k, "readerShortPressSideDown", 2, L);
  expectInt(k, "readerLongPressSideUp", 3, L);
  expectInt(k, "readerLongPressSideDown", 4, L);
  expectInt(k, "readerShortPressPower", 1, L);
  expectInt(k, "readerLongPressConfirm", 10, L);
  expectInt(k, "readerActionsMigrated", 1, L);
  // The plugins master switch maps straight across.
  expectInt(k, "pluginsEnabled", 0, L);
  // Upstream-only keys never leak into ours.
  expectAbsent(k, "lineHeight", L);
  expectAbsent(k, "shortPwrBtn", L);
}

// A KatiePoint document as JsonSettingsIO::saveSettings writes it (subset).
constexpr const char* KATIEPOINT = R"({
  "language": "FR", "sleepTheme": "blank", "seamlessSleepScreen": 0, "refreshFrequency": 2, "uiTheme": 4,
  "pluginsEnabled": 1,
  "fadingFix": 0, "darkMode": 0, "fontFamily": 2, "sdFontFamilyName": "", "dictionaryName": "",
  "fontSize": 3, "lineSpacing": 0, "wordSpacing": 100, "characterSpacing": 2, "screenMargin": 20,
  "paragraphAlignment": 0, "paragraphIndent": 0, "embeddedStyle": 1, "hyphenationEnabled": 0,
  "orientation": 0, "extraParagraphSpacing": 1, "textAntiAliasing": 1, "imageRendering": 1,
  "imageGrayscale": 1, "bionicReading": 0, "sleepTimeout": 4, "showHiddenFiles": 0, "finishedBookFolder": 2,
  "frontButtonFollowOrientation": 0, "statusBarBatteryPos": 0, "statusBarBookTitlePos": 0,
  "statusBarChapterTitlePos": 2, "statusBarBookPercentPos": 3, "statusBarChapterPagePos": 3,
  "statusBarClockPos": 3, "statusBarProgressBar": 2, "statusBarProgressBarThickness": 1,
  "xtcStatusBarMode": 3, "hideBatteryPercentage": 0, "showClockOutsideReader": 0, "clockUtcOffsetQ": 48,
  "clockFormat": 0, "clockTimezone": 255, "clockDst": 0, "frontButtonBack": 0, "frontButtonConfirm": 1,
  "frontButtonLeft": 2, "frontButtonRight": 3, "readerShortPressSideUp": 2, "readerShortPressSideDown": 1,
  "readerLongPressSideUp": 4, "readerLongPressSideDown": 3, "readerShortPressPower": 11,
  "readerLongPressConfirm": 11, "frontlightBrightness": 0, "frontlightLastBrightness": 35,
  "readerActionsMigrated": 1, "statusBarHeaderDatePos": 1, "sortMode": 3, "opdsDownloadFolder": "/Downloads"
})";

void testExport() {
  JsonDocument kp;
  parse(kp, KATIEPOINT);
  JsonDocument up;
  SettingsCompat::exportShared(kp.as<JsonVariantConst>(), up);
  const JsonVariantConst u = up.as<JsonVariantConst>();
  const char* L = "export";

  expectStr(u, "language", "FR", L);
  expectInt(u, "pluginsEnabled", 1, L);
  expectStr(u, "opdsDownloadFolder", "/Downloads", L);
  expectInt(u, "sleepScreen", 5, L);  // blank
  expectInt(u, "quickResumeSleepScreen", 0, L);
  expectInt(u, "refreshFrequency", 2, L);
  expectAbsent(u, "uiTheme", L);  // Carousel: leave upstream's theme alone
  expectInt(u, "screenInverted", 0, L);
  expectInt(u, "fontFamily", 2, L);  // built-in OpenDyslexic -> upstream legacy id
  expectStr(u, "sdFontFamilyName", "", L);
  expectInt(u, "fontSize", 18, L);
  expectInt(u, "lineHeight", 1, L);        // Tight -> +10%
  expectInt(u, "characterSpacing", 1, L);  // -0.5px -> -1px
  expectInt(u, "screenMargin", 20, L);
  expectAbsent(u, "paragraphIndentSpaces", L);  // book's own indent
  expectInt(u, "imageRendering", 1, L);
  expectInt(u, "focusReadingEnabled", 0, L);
  expectInt(u, "sleepTimeoutMinutes", 30, L);
  expectAbsent(u, "moveFinishedToReadFolder", L);  // /Completed
  expectInt(u, "statusBarBattery", 0, L);
  expectInt(u, "statusBarTitle", 1, L);
  expectInt(u, "statusBarBookProgressPercentage", 1, L);
  expectInt(u, "statusBarChapterPageCount", 1, L);
  expectInt(u, "statusBarClock", 1, L);
  expectInt(u, "statusBarProgressBar", 2, L);
  expectInt(u, "xtcStatusBarMode", 1, L);  // Both -> Bottom
  expectInt(u, "hideBatteryPercentage", 0, L);
  expectInt(u, "clockShowHeader", 0, L);
  expectInt(u, "clockTimezone", 255, L);
  expectInt(u, "sideButtonLayout", 0, L);  // Up = back, Down = forward
  expectInt(u, "longPressButtonBehavior", 1, L);
  expectInt(u, "shortPwrBtn", 3, L);            // force refresh
  expectAbsent(u, "longPressMenuFunction", L);  // force refresh: no upstream equivalent
  expectInt(u, "frontlightOn", 0, L);
  expectInt(u, "frontlightBrightness", 35, L);
  // KatiePoint-only keys never leak into settings.json.
  expectAbsent(u, "sleepTheme", L);
  expectAbsent(u, "sortMode", L);
  expectAbsent(u, "statusBarHeaderDatePos", L);
  expectAbsent(u, "readerActionsMigrated", L);
  expectAbsent(u, "darkMode", L);
}

// Every upstream value whose meaning maps must survive import -> export.
void testRoundTrip() {
  struct Case {
    const char* key;
    int from, to;
  };
  constexpr Case CASES[] = {
      {"characterSpacing", 0, 4},      {"fontSize", 12, 18},
      {"refreshFrequency", 0, 4},      {"uiTheme", 0, 2},
      {"paragraphIndentSpaces", 0, 4}, {"imageRendering", 0, 3},
      {"sideButtonLayout", 0, 4},      {"longPressButtonBehavior", 0, 2},
      {"orientation", 0, 3},           {"statusBarClock", 0, 2},
      {"statusBarTitle", 0, 2},        {"lineHeight", 1, 3},
      {"xtcStatusBarMode", 0, 2},      {"pluginsEnabled", 0, 1},
  };
  for (const auto& c : CASES) {
    for (int v = c.from; v <= c.to; v++) {
      if (std::strcmp(c.key, "fontSize") == 0 && (v % 2) != 0) continue;
      JsonDocument up;
      up["pluginsEnabled"] = 0;
      up[c.key] = v;
      JsonDocument kp, back;
      SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
      SettingsCompat::exportShared(kp.as<JsonVariantConst>(), back);
      char label[64];
      std::snprintf(label, sizeof(label), "round trip %s=%d", c.key, v);
      expectInt(back.as<JsonVariantConst>(), c.key, v, label);
    }
  }
  for (int mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
    JsonDocument up;
    up["pluginsEnabled"] = 0;
    up["sleepScreen"] = mode;
    JsonDocument kp, back;
    SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
    SettingsCompat::exportShared(kp.as<JsonVariantConst>(), back);
    char label[64];
    std::snprintf(label, sizeof(label), "round trip sleepScreen=%d", mode);
    expectInt(back.as<JsonVariantConst>(), "sleepScreen", mode, label);
  }
  for (int minutes : {1, 5, 10, 15, 30}) {
    JsonDocument up;
    up["sleepTimeoutMinutes"] = minutes;
    JsonDocument kp, back;
    SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
    SettingsCompat::exportShared(kp.as<JsonVariantConst>(), back);
    expectInt(back.as<JsonVariantConst>(), "sleepTimeoutMinutes", minutes, "round trip sleepTimeoutMinutes");
  }
  for (int power : {0, 2, 3, 4, 6, 7, 8, 9}) {
    JsonDocument up;
    up["pluginsEnabled"] = 0;
    up["shortPwrBtn"] = power;
    JsonDocument kp, back;
    SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
    SettingsCompat::exportShared(kp.as<JsonVariantConst>(), back);
    expectInt(back.as<JsonVariantConst>(), "shortPwrBtn", power, "round trip shortPwrBtn");
  }
  for (int fn = 0; fn <= 6; fn++) {
    JsonDocument up;
    up["pluginsEnabled"] = 0;
    up["longPressMenuFunction"] = fn;
    JsonDocument kp, back;
    SettingsCompat::importUpstream(up.as<JsonVariantConst>(), kp);
    SettingsCompat::exportShared(kp.as<JsonVariantConst>(), back);
    expectInt(back.as<JsonVariantConst>(), "longPressMenuFunction", fn, "round trip longPressMenuFunction");
  }
}
}  // namespace

int main() {
  testDetection();
  testImport();
  testExport();
  testRoundTrip();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("SettingsCompat: all tests passed\n");
  return 0;
}
