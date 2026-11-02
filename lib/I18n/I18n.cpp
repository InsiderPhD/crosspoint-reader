#include "I18n.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <Serialization.h>
#include <strings.h>

#include "I18nStrings.h"

using namespace i18n_strings;

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr const char* SETTINGS_FILE_BAK = "/.crosspoint/language.bin.bak";
static constexpr uint8_t SETTINGS_VERSION = 1;

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // Use generated helper function - no hardcoded switch needed!
  const char* const* strings = getStringArray(_language);
  return strings[index];
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  saveSettings();
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

void I18n::saveSettings() {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    Serial.printf("[I18N] Failed to save settings\n");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writePod(file, static_cast<uint8_t>(_language));

  file.close();
  Serial.printf("[I18N] Settings saved: language=%d\n", static_cast<int>(_language));
}

bool I18n::hasSavedLanguage() { return Storage.exists(SETTINGS_FILE) || Storage.exists(SETTINGS_FILE_BAK); }

const char* I18n::languageCode(const Language lang) {
  const auto index = static_cast<size_t>(lang);
  return index < static_cast<size_t>(Language::_COUNT) ? LANGUAGE_CODES[index] : "EN";
}

bool I18n::languageFromCode(const char* code, Language& out) {
  if (!code) return false;
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcasecmp(code, LANGUAGE_CODES[i]) == 0) {
      out = static_cast<Language>(i);
      return true;
    }
  }
  return false;
}

void I18n::loadSettings() {
  FsFile file;
  // CrossInk imports language.bin and renames it to .bak; after a swap back
  // the .bak still holds this firmware's last choice (same enum numbering).
  if (!Storage.exists(SETTINGS_FILE) && Storage.exists(SETTINGS_FILE_BAK)) {
    if (!Storage.openFileForRead("I18N", SETTINGS_FILE_BAK, file)) return;
  } else if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
    Serial.printf("[I18N] No settings file, keeping current language\n");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != SETTINGS_VERSION) {
    Serial.printf("[I18N] Settings version mismatch\n");
    return;
  }

  uint8_t lang;
  serialization::readPod(file, lang);
  if (lang < static_cast<size_t>(Language::_COUNT)) {
    _language = static_cast<Language>(lang);
    Serial.printf("[I18N] Loaded language: %d\n", static_cast<int>(_language));
  }
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
