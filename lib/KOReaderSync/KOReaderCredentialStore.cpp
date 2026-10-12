#include "KOReaderCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include "../../src/JsonSettingsIO.h"

// Initialize the static instance
KOReaderCredentialStore KOReaderCredentialStore::instance;

namespace {
// File format version (for binary migration)
constexpr uint8_t KOREADER_FILE_VERSION = 1;

// File paths
constexpr char KOREADER_FILE_BIN[] = "/.crosspoint/koreader.bin";
constexpr char KOREADER_FILE_JSON[] = "/.crosspoint/koreader.json";
constexpr char KOREADER_FILE_BAK[] = "/.crosspoint/koreader.bin.bak";

// Default sync server URL
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";

// Legacy obfuscation key - "KOReader" in ASCII (only used for binary migration)
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x4B, 0x4F, 0x52, 0x65, 0x61, 0x64, 0x65, 0x72};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}
}  // namespace

bool KOReaderCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveKOReader(*this, KOREADER_FILE_JSON);
}

bool KOReaderCredentialStore::loadFromFile() {
  // Try JSON first
  if (Storage.exists(KOREADER_FILE_JSON)) {
    String json = Storage.readFile(KOREADER_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadKOReader(*this, json.c_str(), &resave);
      if (result && resave) {
        saveToFile();
        LOG_DBG("KRS", "Resaved KOReader credentials to update format");
      }
      return result;
    }
  }

  // Fall back to binary migration
  if (Storage.exists(KOREADER_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(KOREADER_FILE_BIN, KOREADER_FILE_BAK);
        LOG_DBG("KRS", "Migrated koreader.bin to koreader.json");
        return true;
      } else {
        LOG_ERR("KRS", "Failed to save KOReader credentials during migration");
        return false;
      }
    }
  }

  LOG_DBG("KRS", "No credentials file found");
  return false;
}

bool KOReaderCredentialStore::loadFromBinaryFile() {
  FsFile file;
  if (!Storage.openFileForRead("KRS", KOREADER_FILE_BIN, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != KOREADER_FILE_VERSION) {
    LOG_DBG("KRS", "Unknown file version: %u", version);
    return false;
  }

  // The binary format predates profiles: it is a single account, i.e. profile 0.
  activeProfile = 0;
  std::string& username = profiles[0].username;
  std::string& password = profiles[0].password;
  std::string& serverUrl = profiles[0].serverUrl;

  if (file.available()) {
    serialization::readString(file, username);
  } else {
    username.clear();
  }

  if (file.available()) {
    serialization::readString(file, password);
    legacyDeobfuscate(password);
  } else {
    password.clear();
  }

  if (file.available()) {
    serialization::readString(file, serverUrl);
  } else {
    serverUrl.clear();
  }

  if (file.available()) {
    uint8_t method;
    serialization::readPod(file, method);
    matchMethod = static_cast<DocumentMatchMethod>(method);
  } else {
    matchMethod = DocumentMatchMethod::FILENAME;
  }

  LOG_DBG("KRS", "Loaded KOReader credentials from binary for user: %s", username.c_str());
  return true;
}

bool KOReaderCredentialStore::setActiveProfile(const uint8_t index) {
  if (index >= PROFILE_COUNT) {
    LOG_ERR("KRS", "Profile index out of range: %u", static_cast<unsigned>(index));
    return false;
  }
  activeProfile = index;
  LOG_DBG("KRS", "Active profile: %u", static_cast<unsigned>(index) + 1);
  return true;
}

bool KOReaderCredentialStore::profileHasCredentials(const uint8_t index) const {
  return index < PROFILE_COUNT && !profiles[index].username.empty() && !profiles[index].password.empty();
}

const std::string& KOReaderCredentialStore::getProfileUsername(const uint8_t index) const {
  return profiles[index < PROFILE_COUNT ? index : activeProfile].username;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  current().username = user;
  current().password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  const std::string& password = current().password;
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return profileHasCredentials(activeProfile); }

void KOReaderCredentialStore::clearCredentials() {
  current().username.clear();
  current().password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  current().serverUrl = url;
  LOG_DBG("KRS", "Set server URL: %s", url.empty() ? "(default)" : url.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  const std::string& serverUrl = current().serverUrl;
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else if (serverUrl.find("://") == std::string::npos) {
    // Normalize URL: add http:// if no protocol specified (local servers typically don't have SSL)
    url = "http://" + serverUrl;
  } else {
    url = serverUrl;
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

void KOReaderCredentialStore::setServerType(KOReaderServerType type) {
  // Only the two types this firmware implements are storable.
  if (type != KOReaderServerType::OTHER) {
    type = KOReaderServerType::KOSYNC;
  }
  current().serverType = type;
  LOG_DBG("KRS", "Set server type: %u", static_cast<unsigned>(type));
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}
