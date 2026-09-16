#pragma once

#include <cstdint>
#include <string>

/**
 * Hardcover.app personal API token, plus the account facts every push needs.
 *
 * The token comes from hardcover.app/account/api and is pasted into the web
 * settings page: it is a JWT several hundred characters long, which the
 * on-device keyboard can't reasonably take. Stored XOR-obfuscated with the
 * device MAC, the same (device-tied, not cryptographic) scheme as BookFusion.
 *
 * The account id is cached so a push doesn't spend a request on `me` every
 * time. It belongs to the token, so changing the token clears it.
 *
 * File: /.crosspoint/hardcover.json
 */
class HardcoverTokenStore {
 public:
  static HardcoverTokenStore& getInstance() { return instance; }

  HardcoverTokenStore(const HardcoverTokenStore&) = delete;
  HardcoverTokenStore& operator=(const HardcoverTokenStore&) = delete;

  bool saveToFile() const;
  bool loadFromFile();

  // Accepts the token exactly as hardcover.app shows it ("Bearer eyJ..."), with
  // or without the "Bearer " prefix; surrounding whitespace is trimmed. An empty
  // value clears the token. Persists only when the token actually changed.
  void setToken(const std::string& token);
  const std::string& getToken() const { return token; }
  bool hasToken() const { return !token.empty(); }

  // Account cache from the `me` query; userId 0 = not fetched yet.
  int32_t getUserId() const { return userId; }
  uint8_t getPrivacySettingId() const { return privacySettingId; }
  void setAccount(int32_t id, uint8_t privacy);

 private:
  static HardcoverTokenStore instance;
  HardcoverTokenStore() = default;

  std::string token;
  int32_t userId = 0;
  uint8_t privacySettingId = 1;  // Hardcover's "public"; replaced by the account default
};

#define HC_TOKEN_STORE HardcoverTokenStore::getInstance()
