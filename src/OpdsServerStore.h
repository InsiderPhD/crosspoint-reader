#pragma once
#include <HttpHeader.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "network/HttpDownloader.h"

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with the device key on disk

  // Extra headers sent with every request to this server (e.g. a Cloudflare
  // Access service token). Fixed slots, as upstream CrossPoint: no vector
  // growth on the edit path; an empty name marks an unused slot.
  static constexpr size_t MAX_CUSTOM_HEADERS = 2;
  std::array<HttpHeader, MAX_CUSTOM_HEADERS> customHeaders;

  // CrossInk's per-server "filenameFormat" ("author_title" / "title_author" /
  // "server_filename"). Not used here (the global opdsUseServerFilename
  // setting decides), only carried so a swap back to CrossInk keeps it.
  std::string filenameFormat;

  // Where this catalog's books are saved (CrossInk's per-server
  // "downloadFolder"): empty = the shared default (settings
  // opdsDownloadFolder), "/" = SD root, else a normalised "/A/B".
  std::string downloadFolder;

  // Display name: the given name, else the URL.
  const std::string& label() const { return name.empty() ? url : name; }

  // Headers with a non-empty name, ready to hand to HttpDownloader.
  std::vector<HttpDownloader::Header> activeCustomHeaders() const;
};

/**
 * OPDS catalogs, stored in /.crosspoint/opds.json in the schema upstream
 * CrossPoint 2.0 and CrossInk share ({"servers":[{name,url,username,
 * password_obf,headers[{name,value_obf}]}]}, plus CrossInk's filenameFormat),
 * so the file survives a swap between the three firmwares. Secrets are
 * obfuscated with the device key, as in wifi.json.
 *
 * Loaded on first use, not at boot: the catalog list is only needed by the
 * OPDS screens and the web API. The first load migrates the single server this
 * firmware used to keep in settings (opdsServerUrl/opdsUsername/opdsPassword).
 */
class OpdsServerStore {
 public:
  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore(const OpdsServerStore&) = delete;
  OpdsServerStore& operator=(const OpdsServerStore&) = delete;

  static OpdsServerStore& getInstance() { return instance; }

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const {
    ensureLoaded();
    return servers;
  }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return getServers().size(); }
  bool hasServers() const { return !getServers().empty(); }

 private:
  static OpdsServerStore instance;
  OpdsServerStore() = default;

  void ensureLoaded() const;
  bool loadFromFile();
  bool saveToFile() const;
  bool migrateFromSettings();

  std::vector<OpdsServer> servers;
  bool loaded = false;
};

#define OPDS_STORE OpdsServerStore::getInstance()
