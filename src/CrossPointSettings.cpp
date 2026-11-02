#include "CrossPointSettings.h"

#include <ArduinoJson.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>
#include <string>

#include "SettingsCompat.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_BIN_BAK[] = "/.crosspoint/settings.bin.bak";
// KatiePoint's own settings, so upstream CrossPoint rewriting settings.json
// (it rebuilds the file from its own key list) cannot drop ours. Atomic-save
// scratch and last-known-good copy alongside; see saveToFile().
constexpr char SETTINGS_FILE[] = "/.crosspoint/katiepoint-settings.json";
constexpr char SETTINGS_FILE_TMP[] = "/.crosspoint/katiepoint-settings.json.tmp";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/katiepoint-settings.json.bak";
// Shared with upstream CrossPoint and older KatiePoint builds: imported once
// when our own file does not exist yet, then kept up to date with the settings
// both understand (SettingsCompat).
constexpr char LEGACY_SETTINGS_FILE[] = "/.crosspoint/settings.json";
constexpr char LEGACY_SETTINGS_FILE_TMP[] = "/.crosspoint/settings.json.tmp";
constexpr char LEGACY_SETTINGS_FILE_BAK[] = "/.crosspoint/settings.json.bak";
// Read caps. Our file is ~8KB; upstream's settings.json ~3-4KB.
constexpr size_t SETTINGS_MAX_BYTES = 32 * 1024;
constexpr size_t COMPAT_MAX_BYTES = 16 * 1024;

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

}  // namespace

bool CrossPointSettings::migrateReaderActions(CrossPointSettings& settings) {
  if (settings.readerActionsMigrated) return false;
  settings.readerActionsMigrated = 1;

  // Map legacy longPressAction → readerLongPressConfirm
  switch (static_cast<LONG_PRESS_ACTION>(settings.longPressAction)) {
    case LONG_PRESS_SYNC:
      settings.readerLongPressConfirm = READER_ACTION_SYNC;
      break;
    case LONG_PRESS_PAGE_TURN:
      settings.readerLongPressConfirm = READER_ACTION_PAGE_FORWARD;
      break;
    case LONG_PRESS_NONE:
      settings.readerLongPressConfirm = READER_ACTION_NONE;
      break;
    case LONG_PRESS_BOOKMARK:
      settings.readerLongPressConfirm = READER_ACTION_BOOKMARK;
      break;
    case LONG_PRESS_SLEEP:
      // Sleep is no longer a bindable action; the power long-press still sleeps.
      settings.readerLongPressConfirm = READER_ACTION_NONE;
      break;
    default:  // LONG_PRESS_REFRESH
      settings.readerLongPressConfirm = READER_ACTION_FORCE_REFRESH;
      break;
  }

  // Map legacy shortPwrBtn → readerShortPressPower
  switch (static_cast<SHORT_PWRBTN>(settings.shortPwrBtn)) {
    case SLEEP:
      // See above: the power long-press already sleeps, so the short press is
      // left unbound rather than duplicating it.
      settings.readerShortPressPower = READER_ACTION_NONE;
      break;
    case FORCE_REFRESH:
      settings.readerShortPressPower = READER_ACTION_FORCE_REFRESH;
      break;
    case SHORT_PWRBTN_SYNC:
      settings.readerShortPressPower = READER_ACTION_SYNC;
      break;
    case SHORT_PWRBTN_BOOKMARK:
      settings.readerShortPressPower = READER_ACTION_BOOKMARK;
      break;
    default:  // PAGE_TURN
      settings.readerShortPressPower = READER_ACTION_PAGE_FORWARD;
      break;
  }

  // Map legacy sideButtonLayout → side button actions (NEXT_PREV swaps Up/Down)
  if (settings.sideButtonLayout == NEXT_PREV) {
    settings.readerShortPressSideUp = READER_ACTION_PAGE_FORWARD;
    settings.readerLongPressSideUp = READER_ACTION_SKIP_CHAPTER_FORWARD;
    settings.readerShortPressSideDown = READER_ACTION_PAGE_BACK;
    settings.readerLongPressSideDown = READER_ACTION_SKIP_CHAPTER_BACK;
  }

  // Map legacy longPressChapterSkip = 0 → remove long-press chapter skip
  if (!settings.longPressChapterSkip) {
    settings.readerLongPressSideUp = READER_ACTION_NONE;
    settings.readerLongPressSideDown = READER_ACTION_NONE;
  }

  return true;
}

void CrossPointSettings::migrateStatusBarPositions(CrossPointSettings& settings) {
  // Battery: on -> left, off -> hidden (its classic cluster).
  settings.statusBarBatteryPos = settings.statusBarBattery ? SB_POS_LEFT : SB_POS_HIDE;

  // Title was one-of Book/Chapter/Hide; split into two independent centred elements.
  settings.statusBarBookTitlePos = (settings.statusBarTitle == BOOK_TITLE) ? SB_POS_MIDDLE : SB_POS_HIDE;
  settings.statusBarChapterTitlePos = (settings.statusBarTitle == CHAPTER_TITLE) ? SB_POS_MIDDLE : SB_POS_HIDE;

  // Progress text lived on the right; each half keeps that position when it was on.
  settings.statusBarBookPercentPos = settings.statusBarBookProgressPercentage ? SB_POS_RIGHT : SB_POS_HIDE;
  settings.statusBarChapterPagePos = settings.statusBarChapterPageCount ? SB_POS_RIGHT : SB_POS_HIDE;

  // Time-left was one-of Book/Chapter/Hide; split into two independent right elements.
  settings.statusBarBookTimeLeftPos = (settings.statusBarTimeLeft == TIME_LEFT_BOOK) ? SB_POS_RIGHT : SB_POS_HIDE;
  settings.statusBarChapterTimeLeftPos = (settings.statusBarTimeLeft == TIME_LEFT_CHAPTER) ? SB_POS_RIGHT : SB_POS_HIDE;

  // Clock sat to the left of the progress text on the right.
  settings.statusBarClockPos = settings.statusBarClock ? SB_POS_RIGHT : SB_POS_HIDE;

  // Bookmark indicator is new; default it into the left cluster.
  settings.statusBarBookmarkPos = SB_POS_LEFT;
}

uint8_t& CrossPointSettings::statusBarPosRef(const uint8_t element, const STATUS_BAR_ID bar) {
  if (bar == SB_BAR_HEADER) {
    switch (element) {
      case SB_ELEM_DATE:
        return statusBarHeaderDatePos;
      case SB_ELEM_CLOCK:
        return statusBarHeaderClockPos;
      case SB_ELEM_BATTERY:
        return statusBarHeaderBatteryPos;
      default:
        break;
    }
  } else if (bar == SB_BAR_BOTTOM || bar == SB_BAR_TOP) {
    const bool topBar = bar == SB_BAR_TOP;
    switch (element) {
      case SB_ELEM_BATTERY:
        return topBar ? statusBarTopBatteryPos : statusBarBatteryPos;
      case SB_ELEM_BOOK_TITLE:
        return topBar ? statusBarTopBookTitlePos : statusBarBookTitlePos;
      case SB_ELEM_CHAPTER_TITLE:
        return topBar ? statusBarTopChapterTitlePos : statusBarChapterTitlePos;
      case SB_ELEM_BOOK_PERCENT:
        return topBar ? statusBarTopBookPercentPos : statusBarBookPercentPos;
      case SB_ELEM_CHAPTER_PAGE:
        return topBar ? statusBarTopChapterPagePos : statusBarChapterPagePos;
      case SB_ELEM_BOOK_TIME_LEFT:
        return topBar ? statusBarTopBookTimeLeftPos : statusBarBookTimeLeftPos;
      case SB_ELEM_CHAPTER_TIME_LEFT:
        return topBar ? statusBarTopChapterTimeLeftPos : statusBarChapterTimeLeftPos;
      case SB_ELEM_CLOCK:
        return topBar ? statusBarTopClockPos : statusBarClockPos;
      case SB_ELEM_BOOKMARK:
        return topBar ? statusBarTopBookmarkPos : statusBarBookmarkPos;
      case SB_ELEM_BLUETOOTH:
        return topBar ? statusBarTopBluetoothPos : statusBarBluetoothPos;
      default:
        break;
    }
  }
  // Unknown element or bar, or an element this bar has no slot for: a throwaway
  // slot that always reads as hidden, so a stale index can neither land on a
  // bar nor scribble over a real field.
  static uint8_t unknownElement = SB_POS_HIDE;
  unknownElement = SB_POS_HIDE;
  return unknownElement;
}

uint8_t CrossPointSettings::statusBarPos(const uint8_t element, const STATUS_BAR_ID bar) const {
  const uint8_t pos = const_cast<CrossPointSettings*>(this)->statusBarPosRef(element, bar);
  return pos < STATUS_BAR_POS_COUNT ? pos : static_cast<uint8_t>(SB_POS_HIDE);
}

uint8_t& CrossPointSettings::statusBarBatteryStyleRef(const STATUS_BAR_ID bar) {
  switch (bar) {
    case SB_BAR_TOP:
      return statusBarTopBatteryStyle;
    case SB_BAR_HEADER:
      return statusBarHeaderBatteryStyle;
    default:
      return statusBarBatteryStyle;
  }
}

uint8_t CrossPointSettings::statusBarBatteryStyleOf(const STATUS_BAR_ID bar) const {
  const uint8_t style = const_cast<CrossPointSettings*>(this)->statusBarBatteryStyleRef(bar);
  return style < STATUS_BAR_BATTERY_STYLE_COUNT ? style : static_cast<uint8_t>(SB_BATTERY_ICON_AND_PERCENT);
}

void CrossPointSettings::migrateStatusBarBatteryStyles(CrossPointSettings& settings) {
  // Hide Battery %: Never showed the number everywhere, In Reader hid it on the
  // reader bars only, Always hid it on the header too.
  const uint8_t readerStyle =
      settings.hideBatteryPercentage == HIDE_NEVER ? SB_BATTERY_ICON_AND_PERCENT : SB_BATTERY_ICON_ONLY;
  settings.statusBarBatteryStyle = readerStyle;
  settings.statusBarTopBatteryStyle = readerStyle;
}

void CrossPointSettings::migrateHeaderStatusBar(CrossPointSettings& settings) {
  // The classic header: date on the left with the optional clock right after
  // it, battery on the right.
  settings.statusBarHeaderDatePos = SB_POS_LEFT;
  settings.statusBarHeaderClockPos = settings.showClockOutsideReader ? SB_POS_LEFT : SB_POS_HIDE;
  settings.statusBarHeaderBatteryPos = SB_POS_RIGHT;
  settings.statusBarHeaderBatteryStyle =
      settings.hideBatteryPercentage == HIDE_ALWAYS ? SB_BATTERY_ICON_ONLY : SB_BATTERY_ICON_AND_PERCENT;
}

uint8_t CrossPointSettings::legacyHideBatteryPercentage() const {
  // Closest older-firmware equivalent of the three styles. "% Only" has none
  // and reads as "shown".
  if (statusBarBatteryStyleOf(SB_BAR_HEADER) == SB_BATTERY_ICON_ONLY) return HIDE_ALWAYS;
  if (statusBarBatteryStyleOf(SB_BAR_BOTTOM) == SB_BATTERY_ICON_ONLY) return HIDE_READER;
  return HIDE_NEVER;
}

bool CrossPointSettings::statusBarElementPlaced(const uint8_t element) const {
  return statusBarPos(element, false) != SB_POS_HIDE || statusBarPos(element, true) != SB_POS_HIDE;
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

void CrossPointSettings::sanitizeReaderCombos(CrossPointSettings& settings) {
  // Bits above the last known button came from a newer firmware (or a corrupt
  // file); dropping them can only shrink a mask, never rebind it elsewhere.
  constexpr uint16_t VALID_BITS = static_cast<uint16_t>((1u << COMBO_BUTTON_COUNT) - 1u);
  for (uint8_t slot = 0; slot < READER_COMBO_SLOTS; slot++) {
    uint16_t& mask = settings.readerComboButtons[slot];
    uint8_t& action = settings.readerComboAction[slot];
    mask &= VALID_BITS;
    if (action >= READER_ACTION_COUNT || isRetiredReaderAction(action)) {
      action = READER_ACTION_NONE;
    }
    // A chord is two or more keys by definition, and a slot bound to nothing
    // would swallow its buttons for no gain — clear both halves together so
    // "empty" has exactly one representation.
    if (__builtin_popcount(mask) < 2 || action == READER_ACTION_NONE) {
      mask = 0;
      action = READER_ACTION_NONE;
      continue;
    }
    // Duplicate masks: the first slot wins at dispatch, so the later one is
    // dead weight that still swallows its buttons. Clear it.
    for (uint8_t earlier = 0; earlier < slot; earlier++) {
      if (settings.readerComboButtons[earlier] == mask) {
        mask = 0;
        action = READER_ACTION_NONE;
        break;
      }
    }
  }
}

void CrossPointSettings::sanitizeReaderActions(CrossPointSettings& settings) {
  // Static table of the assignable slots. Member pointers are compile-time
  // constants, so the array lives in flash rather than costing DRAM.
  static constexpr uint8_t CrossPointSettings::* SLOTS[] = {
      &CrossPointSettings::readerShortPressBack,     &CrossPointSettings::readerLongPressBack,
      &CrossPointSettings::readerShortPressConfirm,  &CrossPointSettings::readerLongPressConfirm,
      &CrossPointSettings::readerShortPressLeft,     &CrossPointSettings::readerLongPressLeft,
      &CrossPointSettings::readerShortPressRight,    &CrossPointSettings::readerLongPressRight,
      &CrossPointSettings::readerShortPressSideUp,   &CrossPointSettings::readerLongPressSideUp,
      &CrossPointSettings::readerShortPressSideDown, &CrossPointSettings::readerLongPressSideDown,
      &CrossPointSettings::readerShortPressPower,    &CrossPointSettings::readerTapLeft,
      &CrossPointSettings::readerTapMiddle,          &CrossPointSettings::readerTapRight,
      &CrossPointSettings::readerHoldLeft,           &CrossPointSettings::readerHoldMiddle,
      &CrossPointSettings::readerHoldRight,          &CrossPointSettings::readerShortPressHome,
      &CrossPointSettings::readerLongPressHome,      &CrossPointSettings::readerDoubleTapHome,
  };
  for (const auto slot : SLOTS) {
    uint8_t& action = settings.*slot;
    if (action >= READER_ACTION_COUNT || isRetiredReaderAction(action)) {
      action = READER_ACTION_NONE;
    }
  }
}

void CrossPointSettings::sanitizeFrontlightSchedules(CrossPointSettings& settings) {
  constexpr uint16_t MINUTES_PER_DAY = 24 * 60;
  for (auto& slot : settings.frontlightSchedules) {
    slot.enabled = slot.enabled ? 1 : 0;
    if (slot.startMinutes >= MINUTES_PER_DAY) slot.startMinutes = 0;
    if (slot.endMinutes >= MINUTES_PER_DAY) slot.endMinutes = 0;
    if (slot.brightness > 100) slot.brightness = 100;
    if (slot.warmth > 100) slot.warmth = 100;
    slot.darkMode = slot.darkMode ? 1 : 0;
  }
  if (settings.frontlightScheduleActive > FRONTLIGHT_SCHEDULE_SLOTS) settings.frontlightScheduleActive = 0;
  if (settings.frontlightScheduleRestoreBrightness > 100) settings.frontlightScheduleRestoreBrightness = 100;
  if (settings.frontlightScheduleRestoreWarmth > 100) settings.frontlightScheduleRestoreWarmth = 100;
  settings.frontlightScheduleRestoreDarkMode = settings.frontlightScheduleRestoreDarkMode ? 1 : 0;
}

bool CrossPointSettings::isUnavailableReaderAction(const uint8_t action) {
  return action == READER_ACTION_TOGGLE_FRONTLIGHT && !halFrontlight.present();
}

namespace {

// FNV-1a over the serialised bytes, fed by serializeJson() as a custom writer
// so hashing the compatibility payload allocates nothing.
struct HashWriter {
  uint32_t hash = 2166136261u;
  size_t write(const uint8_t c) {
    hash = (hash ^ c) * 16777619u;
    return 1;
  }
  size_t write(const uint8_t* s, const size_t n) {
    for (size_t i = 0; i < n; i++) write(s[i]);
    return n;
  }
};

// Hash of the last compatibility payload written to (or found already in)
// settings.json this session. 0 = not checked yet. Static, not heap.
uint32_t lastCompatHash = 0;

// Publish a fully written scratch file over `path`. FAT has no
// replace-by-rename (SdFat's rename refuses an existing target), so the
// previous file is parked as `bak` first; on failure it is put back.
bool publishScratch(const char* tmp, const char* path, const char* bak, const bool keepBak) {
  const bool hadPrevious = Storage.exists(path);
  if (hadPrevious) {
    if (Storage.exists(bak) && !Storage.remove(bak)) {
      LOG_ERR("CPS", "Could not drop the old %s", bak);
      Storage.remove(tmp);
      return false;
    }
    if (!Storage.rename(path, bak)) {
      LOG_ERR("CPS", "Could not park %s as %s", path, bak);
      Storage.remove(tmp);
      return false;
    }
  }
  if (!Storage.rename(tmp, path)) {
    LOG_ERR("CPS", "Could not publish %s", tmp);
    if (hadPrevious && !Storage.rename(bak, path)) {
      // The loader promotes the .bak when the main file is missing.
      LOG_ERR("CPS", "Previous settings remain at %s", bak);
    }
    Storage.remove(tmp);
    return false;
  }
  if (hadPrevious && !keepBak) Storage.remove(bak);
  return true;
}

// Finish (or undo) a save interrupted between its two renames: `path` is gone,
// and either the complete scratch file or the parked previous file is still
// there. The scratch file is the newer of the two.
void recoverInterruptedSave(const char* path, const char* tmp, const char* bak) {
  if (Storage.exists(path)) return;
  if (Storage.exists(tmp) && Storage.rename(tmp, path)) {
    LOG_INF("CPS", "Recovered %s from an interrupted save", path);
  } else if (Storage.exists(bak) && Storage.rename(bak, path)) {
    LOG_INF("CPS", "Restored %s from %s", path, bak);
  }
}

// Apply a "language" code read from a settings document. language.bin stays
// the source of truth here, so the code is used only when neither it nor
// CrossInk's language.bin.bak is on the card — or, for a settings.json written by
// upstream CrossPoint, always: that firmware keeps the user's current choice
// there, and any language.bin left on the card predates it.
void applyStoredLanguage(JsonVariantConst code, const bool overrideSaved) {
  Language lang;
  if (!I18n::languageFromCode(code.as<const char*>(), lang)) return;
  if (!overrideSaved && I18n::hasSavedLanguage()) return;
  if (lang != I18N.getLanguage() || !I18n::hasSavedLanguage()) I18N.setLanguage(lang);  // writes language.bin
}

// Write `shared` (upstream key names and numbering, from
// SettingsCompat::exportShared) into settings.json for upstream CrossPoint.
//
// Heap: everything here is transient and freed before returning — the
// existing file (capped at COMPAT_MAX_BYTES), its parsed document, two small
// documents for the round-trip baseline and the output string, roughly 4x the
// file size (~16KB at upstream's typical 3-4KB) at peak. It runs only after a
// successful settings save (already value-change guarded by the callers) and
// only when the shared payload hash moved, so it is not on any per-page path.
// A stack buffer was rejected: 16KB is far over the 256-byte stack budget and
// the document size is not fixed. Any allocation failure skips the write.
void writeCompatSettings(const JsonDocument& shared) {
  HashWriter hasher;
  serializeJson(shared, hasher);
  if (hasher.hash == lastCompatHash) return;

  std::string existing;
  if (Storage.exists(LEGACY_SETTINGS_FILE)) {
    FsFile probe;
    size_t size = 0;
    if (Storage.openFileForRead("CPS", LEGACY_SETTINGS_FILE, probe)) {
      size = probe.fileSize();
      probe.close();
    }
    if (size > COMPAT_MAX_BYTES) {
      LOG_ERR("CPS", "settings.json is %u bytes; not updating it", static_cast<unsigned>(size));
      return;
    }
    if (size > 0 && !Storage.readFileToString("CPS", LEGACY_SETTINGS_FILE, COMPAT_MAX_BYTES, existing)) {
      LOG_ERR("CPS", "Could not read settings.json; not updating it");
      return;
    }
  }

  JsonDocument merged;
  if (!existing.empty()) {
    const DeserializationError err = deserializeJson(merged, existing);
    if (err) {
      LOG_ERR("CPS", "settings.json does not parse (%s); not updating it", err.c_str());
      return;
    }
    if (!SettingsCompat::isUpstreamDocument(merged)) {
      // Written by an older KatiePoint, in this firmware's numbering: nothing
      // in it is safe to hand upstream as-is, so start from the shared keys.
      merged.clear();
    }
  }

  if (!merged.isNull()) {
    // Keep upstream's exact value for every setting not changed here since it
    // was imported: translate the file to our numbering and back, and leave a
    // key alone when that round trip already gives what we would write. This
    // stops lossy mappings (Cover Grid theme, "Never" refresh, odd point
    // sizes) overwriting upstream's choice when the user never touched them.
    JsonDocument baseline;
    {
      JsonDocument imported;
      SettingsCompat::importUpstream(merged.as<JsonVariantConst>(), imported);
      SettingsCompat::exportShared(imported.as<JsonVariantConst>(), baseline);
    }
    // A key the round trip does not produce at all is one whose upstream value
    // has no counterpart here (Cover Grid, Cover + Transparent sleep screen):
    // upstream's choice is kept rather than replaced by our default.
    for (const JsonPairConst kv : shared.as<JsonObjectConst>()) {
      const JsonVariantConst current = merged[kv.key()];
      const JsonVariantConst base = baseline[kv.key()];
      if (!current.isNull() && (base.isNull() || base == kv.value())) continue;
      merged[kv.key()] = kv.value();
    }
  } else {
    merged.set(shared.as<JsonVariantConst>());
  }
  if (merged.overflowed()) {
    LOG_ERR("CPS", "Out of memory building settings.json; not updating it");
    return;
  }

  String out;
  serializeJson(merged, out);
  if (out.length() != measureJson(merged)) {
    LOG_ERR("CPS", "Out of memory serialising settings.json; not updating it");
    return;
  }
  if (existing.size() == out.length() && memcmp(existing.data(), out.c_str(), existing.size()) == 0) {
    lastCompatHash = hasher.hash;  // already up to date: no SD write
    return;
  }
  if (!Storage.writeFile(LEGACY_SETTINGS_FILE_TMP, out)) {
    LOG_ERR("CPS", "Failed to write %s", LEGACY_SETTINGS_FILE_TMP);
    Storage.remove(LEGACY_SETTINGS_FILE_TMP);
    return;
  }
  if (publishScratch(LEGACY_SETTINGS_FILE_TMP, LEGACY_SETTINGS_FILE, LEGACY_SETTINGS_FILE_BAK, false)) {
    lastCompatHash = hasher.hash;
    LOG_DBG("CPS", "Updated settings.json for other firmware");
  }
}

}  // namespace

bool CrossPointSettings::saveToFile() const {
  if (frontlightBrightness > 0) frontlightLastBrightness = frontlightBrightness;
  Storage.mkdir("/.crosspoint");

  // Never write the settings file in place: a power cut or card error mid-write
  // would leave a truncated document, which fails to parse and silently resets
  // every setting on the next boot. The whole document goes to a scratch file
  // first; only a complete, closed file is published, and the previous one is
  // left behind as .bak, the last known good copy for loadFromFile().
  //
  // The shared subset for settings.json is derived from the same document
  // while it is in memory (a small extra document, ~2KB), so the full one is
  // built once per save as before.
  JsonDocument shared;
  {
    JsonDocument doc;
    JsonSettingsIO::serializeSettings(*this, doc);
    String json;
    serializeJson(doc, json);
    if (json.length() == 0 || !Storage.writeFile(SETTINGS_FILE_TMP, json)) {
      LOG_ERR("CPS", "Failed to write %s; settings left untouched", SETTINGS_FILE_TMP);
      Storage.remove(SETTINGS_FILE_TMP);
      return false;
    }
    SettingsCompat::exportShared(doc.as<JsonVariantConst>(), shared);
  }
  if (!publishScratch(SETTINGS_FILE_TMP, SETTINGS_FILE, SETTINGS_FILE_BAK, true)) return false;

  // Best effort: a failure here is logged and never fails this save.
  if (!shared.overflowed()) writeCompatSettings(shared);
  return true;
}

bool CrossPointSettings::loadOwnFile() {
  // Our own file exists (or did, as .tmp / .bak), so it is authoritative even
  // when damaged: falling through would import another firmware's settings.json
  // over the user's KatiePoint preferences (CrossInk follows the same rule).
  recoverInterruptedSave(SETTINGS_FILE, SETTINGS_FILE_TMP, SETTINGS_FILE_BAK);

  bool resave = false;
  bool result = false;
  JsonDocument doc;
  for (const char* path : {SETTINGS_FILE, SETTINGS_FILE_BAK}) {
    std::string json;
    if (!Storage.readFileToString("CPS", path, SETTINGS_MAX_BYTES, json)) continue;
    doc.clear();
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
      LOG_ERR("CPS", "%s does not parse: %s", path, err.c_str());
      continue;
    }
    result = JsonSettingsIO::loadSettings(*this, doc, &resave);
    if (result) {
      if (path == SETTINGS_FILE_BAK) {
        // Fell back to the copy the last successful save left behind; rewrite
        // the main file from it.
        LOG_ERR("CPS", "%s is unreadable; loaded %s", SETTINGS_FILE, SETTINGS_FILE_BAK);
        resave = true;
      }
      break;
    }
  }
  if (!result) return false;

  applyStoredLanguage(doc.as<JsonVariantConst>()["language"], false);
  // Only write back when something actually changed: both migrations self-gate
  // on a "migrated" flag a loaded file already carries, so a normal boot writes
  // nothing.
  if (migrateReaderActions(*this)) resave = true;
  if (resave) {
    if (saveToFile()) {
      LOG_DBG("CPS", "Resaved settings to update format");
    } else {
      LOG_ERR("CPS", "Failed to resave settings after format update");
    }
  }
  return true;
}

bool CrossPointSettings::importLegacyJson() {
  // settings.json is shared with upstream CrossPoint (and older KatiePoint
  // builds), so it is only read here, never moved: the other firmware still
  // owns it.
  recoverInterruptedSave(LEGACY_SETTINGS_FILE, LEGACY_SETTINGS_FILE_TMP, LEGACY_SETTINGS_FILE_BAK);
  if (!Storage.exists(LEGACY_SETTINGS_FILE)) return false;

  bool loaded = false;
  {
    // Transient import buffers (boot only, freed at the end of this scope).
    std::string json;
    if (!Storage.readFileToString("CPS", LEGACY_SETTINGS_FILE, SETTINGS_MAX_BYTES, json)) {
      LOG_ERR("CPS", "settings.json unreadable; not importing it");
      return false;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err) {
      LOG_ERR("CPS", "settings.json does not parse (%s); not importing it", err.c_str());
      return false;
    }
    json.clear();
    json.shrink_to_fit();

    if (SettingsCompat::isUpstreamDocument(doc)) {
      JsonDocument translated;
      SettingsCompat::importUpstream(doc.as<JsonVariantConst>(), translated);
      loaded = JsonSettingsIO::loadSettings(*this, translated);
      if (loaded) applyStoredLanguage(translated.as<JsonVariantConst>()["language"], true);
      LOG_INF("CPS", "Imported upstream CrossPoint settings.json");
    } else {
      // An older KatiePoint build's own file: this firmware's numbering.
      loaded = JsonSettingsIO::loadSettings(*this, doc);
      if (loaded) migrateReaderActions(*this);
      LOG_INF("CPS", "Imported KatiePoint settings.json");
    }
  }
  if (!loaded) return false;
  if (!saveToFile()) LOG_ERR("CPS", "Failed to save imported settings to %s", SETTINGS_FILE);
  return true;
}

bool CrossPointSettings::loadFromFile() {
  if (Storage.exists(SETTINGS_FILE) || Storage.exists(SETTINGS_FILE_TMP) || Storage.exists(SETTINGS_FILE_BAK)) {
    return loadOwnFile();
  }
  if (importLegacyJson()) return true;

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      // settings.bin predates the per-bar battery styles and the header bar.
      migrateStatusBarBatteryStyles(*this);
      migrateHeaderStatusBar(*this);
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BIN_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to %s", SETTINGS_FILE);
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  uint8_t legacySleepMode = LEGACY_SLEEP_DARK;
  uint8_t retiredValue = 0;  // fields read only to keep the file in step
  do {
    serialization::readPod(inputFile, legacySleepMode);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, retiredValue);  // sleep cover mode
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, retiredValue);  // sleep cover filter
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, uiTheme);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, frontButtonFollowOrientation);
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  (void)retiredValue;
  if (const char* theme = legacySleepThemeFor(legacySleepMode)) {
    strncpy(sleepTheme, theme, sizeof(sleepTheme) - 1);
    sleepTheme[sizeof(sleepTheme) - 1] = '\0';
  }

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

TextLayoutOptions CrossPointSettings::readerTextLayout() const {
  TextLayoutOptions options;
  if (characterSpacing < CHARACTER_SPACING_COUNT) {
    options.characterSpacing = static_cast<int8_t>(static_cast<int>(characterSpacing) - CHARACTER_SPACING_ZERO);
  }
  if (wordSpacing >= WORD_SPACING_MIN && wordSpacing <= WORD_SPACING_MAX) {
    options.wordSpacingPercent = wordSpacing;
  }
  if (paragraphIndent < TextLayoutOptions::PARAGRAPH_INDENT_COUNT) {
    options.paragraphIndent = paragraphIndent;
  }
#if CROSSPOINT_EPUB_BLOCK_FONT_SIZES
  // Only the built-in families ship every size. An SD-card or TrueType family is loaded at
  // one size, so the ladder stays empty and headings keep the reader font.
  const int readerFontId = getReaderFontId();
  static_assert(TextLayoutOptions::FONT_SIZE_STEPS == FONT_SIZE_COUNT, "one ladder step per font size setting");
  bool builtIn = false;
  for (uint8_t size = 0; size < FONT_SIZE_COUNT; ++size) {
    options.fontSizeLadder[size] = builtInReaderFontId(fontFamily, size);
    builtIn = builtIn || options.fontSizeLadder[size] == readerFontId;
  }
  if (!builtIn) {
    for (int& id : options.fontSizeLadder) id = 0;
  }
#endif
  return options;
}

float CrossPointSettings::getReaderLineCompression() const {
  switch (fontFamily) {
    case BOOKERLY:
    default:
      switch (lineSpacing) {
        case TIGHT:
          return 0.95f;
        case NORMAL:
        default:
          return 1.0f;
        case WIDE:
          return 1.1f;
      }
    case INTER:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
    case OPENDYSLEXIC:
      switch (lineSpacing) {
        case TIGHT:
          return 0.82f;
        case NORMAL:
        default:
          return 0.87f;
        case WIDE:
          return 0.95f;
      }
    case MONOSPACE:
      switch (lineSpacing) {
        case TIGHT:
          return 0.90f;
        case NORMAL:
        default:
          return 0.95f;
        case WIDE:
          return 1.0f;
      }
  }
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
    default:
      return 10UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

int CrossPointSettings::getCodeFontId() const {
  // Sizes match Bookerly (6/8/10/12) so code blocks read at the same scale as body text.
  switch (fontSize) {
    case SMALL:
      return MONO_6_FONT_ID;
    case MEDIUM:
    default:
      return MONO_8_FONT_ID;
    case LARGE:
      return MONO_10_FONT_ID;
    case EXTRA_LARGE:
      return MONO_12_FONT_ID;
  }
}

int CrossPointSettings::builtInReaderFontId(const uint8_t family, const uint8_t size) {
  // All families normalised to the same point sizes (6/8/10/12) so switching family
  // or size keeps text at a consistent scale. Rows: FONT_FAMILY, columns: FONT_SIZE.
  static constexpr int FONT_IDS[FONT_FAMILY_COUNT][FONT_SIZE_COUNT] = {
      {BOOKERLY_6_FONT_ID, BOOKERLY_8_FONT_ID, BOOKERLY_10_FONT_ID, BOOKERLY_12_FONT_ID},
      {INTER_6_FONT_ID, INTER_8_FONT_ID, INTER_10_FONT_ID, INTER_12_FONT_ID},
      {OPENDYSLEXIC_6_FONT_ID, OPENDYSLEXIC_8_FONT_ID, OPENDYSLEXIC_10_FONT_ID, OPENDYSLEXIC_12_FONT_ID},
      {MONO_6_FONT_ID, MONO_8_FONT_ID, MONO_10_FONT_ID, MONO_12_FONT_ID},
  };
  // Out-of-range values fall back as the old switch did: Bookerly, Medium.
  return FONT_IDS[family < FONT_FAMILY_COUNT ? family : BOOKERLY][size < FONT_SIZE_COUNT ? size : MEDIUM];
}

int CrossPointSettings::getReaderFontId() const {
  // SD card font selection overrides the built-in fontFamily enum.
  if (sdFontFamilyName[0] != '\0' && sdFontIdResolver) {
    int id = sdFontIdResolver(sdFontResolverCtx, sdFontFamilyName, fontSize);
    if (id != 0) return id;
    // SD font failed to load — fall through to built-in below.
  }
  return builtInReaderFontId(fontFamily, fontSize);
}

const char* CrossPointSettings::legacySleepThemeFor(const uint8_t mode) {
  switch (mode) {
    case LEGACY_SLEEP_LIGHT:
      return "light";
    case LEGACY_SLEEP_CUSTOM:
      return "custom";
    case LEGACY_SLEEP_COVER:
      // Fit and Crop alike: the store's cover theme fills the screen.
      return "cover-crop";
    case LEGACY_SLEEP_BLANK:
      return "blank";
    case LEGACY_SLEEP_COVER_CUSTOM:
      return "cover-or-custom";
    case LEGACY_SLEEP_OVERLAY:
      return "page-overlay";
    case LEGACY_SLEEP_THEME:
      return nullptr;
    case LEGACY_SLEEP_DARK:
    default:
      return "dark";
  }
}
