#pragma once
#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// What the server accepts beyond the standard KOSync payload. The
// numbers are upstream CrossPoint's (koreader.json "serverType"); never renumber.
enum class KOReaderServerType : uint8_t {
  CROSSPOINT = 0,  // Upstream's rich-position server. Not implemented here: loads as KOSYNC.
  KOSYNC = 1,      // Strict standard KOSync payloads.
  OTHER = 2,       // Custom server that also takes the extended metadata fields.
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
 */
class KOReaderCredentialStore {
 private:
  static KOReaderCredentialStore instance;
  std::string username;
  std::string password;
  std::string serverUrl;                                            // Custom sync server URL (empty = default)
  KOReaderServerType serverType = KOReaderServerType::KOSYNC;       // What kind of server serverUrl points at
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  // Privacy switch for title/author/filename (and, on an OTHER server, ISBN and
  // series). Off by default: the book you are reading is nobody's business
  // unless you say so.
  bool sendMetadata = false;

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

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const { return username; }
  const std::string& getPassword() const { return password; }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Clear credentials
  void clearCredentials();

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const { return serverUrl; }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Server capability
  void setServerType(KOReaderServerType type);
  KOReaderServerType getServerType() const { return serverType; }
  bool supportsExtendedMetadata() const { return serverType == KOReaderServerType::OTHER; }

  // Whether progress uploads carry document metadata at all
  void setSendMetadata(bool enabled) { sendMetadata = enabled; }
  bool getSendMetadata() const { return sendMetadata; }

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
