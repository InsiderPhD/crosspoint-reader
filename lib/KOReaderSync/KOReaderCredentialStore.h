#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "HttpHeader.h"

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// What the server accepts beyond the standard KOSync payload. The
// numbers are upstream CrossPoint's (koreader.json "serverType"); never renumber.
enum class KOReaderServerType : uint8_t {
  CROSSPOINT = 0,  // crosspoint-sync: standard payload plus the rich `position` object.
  KOSYNC = 1,      // Strict standard KOSync payloads.
  OTHER = 2,       // Custom server that takes rich position and the extended metadata fields.
};

// How a manual sync resolves a difference after fetching remote progress.
// Numbers are upstream's (koreader.json "syncBehavior"); never renumber.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Always show the Apply/Upload choice.
  SMART = 1,           // Auto-resolve the simple cases: furthest progress wins.
};

// Extra request headers (e.g. a Cloudflare Access service token). Fixed-size so
// editing never grows a vector; an empty name marks an unused slot.
constexpr size_t KOREADER_MAX_CUSTOM_HEADERS = 2;

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
  KOReaderServerType serverType = KOReaderServerType::CROSSPOINT;   // What kind of server serverUrl points at
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  // Privacy switch for title/author/filename (and, on an OTHER server, ISBN and
  // series). Off by default: the book you are reading is nobody's business
  // unless you say so.
  bool sendMetadata = false;
  // Opt-in crosspoint-sync extension: a manual sync also pushes and pulls the
  // book's clippings. Needs a server with the /api/v1/clippings endpoint.
  bool syncClippings = false;
  // New setups get Smart; a file saved before the setting existed keeps Ask.
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::SMART;
  std::array<HttpHeader, KOREADER_MAX_CUSTOM_HEADERS> customHeaders;

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
  // True when requests go to the CrossPoint server (empty URL or that URL typed in).
  bool usesDefaultServer() const;

  // Server capability
  void setServerType(KOReaderServerType type);
  KOReaderServerType getServerType() const { return serverType; }
  bool supportsExtendedMetadata() const { return serverType == KOReaderServerType::OTHER; }
  // The rich `position` object goes to every server type but strict KOSync.
  bool supportsRichProgress() const { return serverType != KOReaderServerType::KOSYNC; }

  // Whether progress uploads carry document metadata at all
  void setSendMetadata(bool enabled) { sendMetadata = enabled; }
  bool getSendMetadata() const { return sendMetadata; }

  // Whether a manual sync also syncs clippings
  void setSyncClippings(bool enabled) { syncClippings = enabled; }
  bool getSyncClippings() const { return syncClippings; }

  // Manual sync conflict resolution
  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const { return syncBehavior; }

  // Custom headers sent with every sync request (index must be < KOREADER_MAX_CUSTOM_HEADERS)
  void setCustomHeader(size_t index, const std::string& name, const std::string& value);
  const std::array<HttpHeader, KOREADER_MAX_CUSTOM_HEADERS>& getCustomHeaders() const { return customHeaders; }

  // URL a settings file with no serverUrl used before the default moved to the
  // CrossPoint server. Loading such a file pins it, so an existing account is
  // never silently pointed at a server it doesn't exist on.
  static const char* legacyDefaultServerUrl();

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const { return matchMethod; }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
