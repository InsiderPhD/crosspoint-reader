#pragma once
#include <array>
#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

class KOReaderCredentialStore;
namespace JsonSettingsIO {
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 *
 * Server profiles: PROFILE_COUNT fixed slots, each its own server URL and
 * account, and one active slot. The slot index is the profile's identity (it is
 * what koreader.json and the per-book sync sidecars record), so slots are never
 * reordered or compacted. Every credential accessor below reads the profile
 * that is in effect, which today is always the globally active one.
 *
 * Per-book override (not built): the per-book sidecar written by
 * KOReaderSyncStateStore is the place for it. A later change stores an override
 * index there and makes current() consult it (set on book open, cleared on
 * close) before falling back to activeProfile; no caller of this class changes.
 *
 * Resident cost: the two extra slots are six empty std::strings (~150 bytes)
 * until the user fills them in.
 */
class KOReaderCredentialStore {
 public:
  static constexpr uint8_t PROFILE_COUNT = 3;

 private:
  struct Profile {
    std::string username;
    std::string password;
    std::string serverUrl;  // Custom sync server URL (empty = default)
  };

  static KOReaderCredentialStore instance;
  std::array<Profile, PROFILE_COUNT> profiles;
  uint8_t activeProfile = 0;
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility

  // The profile in effect. The single point a per-book override would hook.
  Profile& current() { return profiles[activeProfile]; }
  const Profile& current() const { return profiles[activeProfile]; }

  // Private constructor for singleton
  KOReaderCredentialStore() = default;

  bool loadFromBinaryFile();

  friend bool JsonSettingsIO::saveKOReader(const KOReaderCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadKOReader(KOReaderCredentialStore&, const char*, bool*);

 public:
  // Delete copy constructor and assignment
  KOReaderCredentialStore(const KOReaderCredentialStore&) = delete;
  KOReaderCredentialStore& operator=(const KOReaderCredentialStore&) = delete;

  // Get singleton instance
  static KOReaderCredentialStore& getInstance() { return instance; }

  // Save/load from SD card
  bool saveToFile() const;
  bool loadFromFile();

  // Server profiles. Switching does not save; call saveToFile().
  uint8_t getActiveProfile() const { return activeProfile; }
  bool setActiveProfile(uint8_t index);
  // For profile pickers: whether a slot has an account, and its username.
  // Passwords are only ever reachable for the profile in effect.
  bool profileHasCredentials(uint8_t index) const;
  const std::string& getProfileUsername(uint8_t index) const;

  // Credential management (profile in effect)
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return current().username; }
  const std::string& getPassword() const { return current().password; }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Clear credentials
  void clearCredentials();

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return current().serverUrl; }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
