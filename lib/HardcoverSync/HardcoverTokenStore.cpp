#include "HardcoverTokenStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <strings.h>

#include <cctype>

HardcoverTokenStore HardcoverTokenStore::instance;

namespace {
constexpr char HC_FILE_JSON[] = "/.crosspoint/hardcover.json";
constexpr char BEARER_PREFIX[] = "Bearer ";
}  // namespace

bool HardcoverTokenStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");

  JsonDocument doc;
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["user_id"] = userId;
  doc["privacy_setting_id"] = privacySettingId;

  String json;
  serializeJson(doc, json);
  const bool ok = Storage.writeFile(HC_FILE_JSON, json);
  if (!ok) {
    LOG_ERR("HCS", "Failed to save %s", HC_FILE_JSON);
  }
  return ok;
}

bool HardcoverTokenStore::loadFromFile() {
  if (!Storage.exists(HC_FILE_JSON)) {
    LOG_DBG("HCS", "No Hardcover token file");
    return false;
  }

  String json = Storage.readFile(HC_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("HCS", "Token file JSON parse error: %s", error.c_str());
    return false;
  }

  bool ok = false;
  std::string loaded = obfuscation::deobfuscateFromBase64(doc["token_obf"] | "", &ok);
  if (!ok) {
    LOG_ERR("HCS", "Token file could not be decoded");
    return false;
  }
  token = std::move(loaded);
  userId = doc["user_id"] | 0;
  privacySettingId = doc["privacy_setting_id"] | 1;
  LOG_DBG("HCS", "Loaded Hardcover token (%zu chars), user %d", token.size(), static_cast<int>(userId));
  return true;
}

void HardcoverTokenStore::setToken(const std::string& value) {
  size_t start = 0;
  size_t end = value.size();
  while (start < end && isspace(static_cast<unsigned char>(value[start]))) start++;
  while (end > start && isspace(static_cast<unsigned char>(value[end - 1]))) end--;

  constexpr size_t prefixLen = sizeof(BEARER_PREFIX) - 1;
  if (end - start >= prefixLen && strncasecmp(value.c_str() + start, BEARER_PREFIX, prefixLen) == 0) {
    start += prefixLen;
    while (start < end && isspace(static_cast<unsigned char>(value[start]))) start++;
  }

  std::string normalised = value.substr(start, end - start);
  if (normalised == token) {
    return;
  }
  token = std::move(normalised);
  userId = 0;
  privacySettingId = 1;
  saveToFile();
  LOG_DBG("HCS", "Hardcover token %s (%zu chars)", token.empty() ? "cleared" : "set", token.size());
}

void HardcoverTokenStore::setAccount(const int32_t id, const uint8_t privacy) {
  if (id == userId && privacy == privacySettingId) {
    return;
  }
  userId = id;
  privacySettingId = privacy;
  saveToFile();
}
