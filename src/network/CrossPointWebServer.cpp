#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>  // outbound TLS for the Libby relay/fetch
#include <Util.h>              // freeink::content::base64Decode
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "SdCardFontGlobals.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "activities/home/LibraryActivity.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/LibbyPageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "html/js/libbyCryptoJs.generated.h"

namespace {
// Arduino's WebServer keeps parsed request arguments alive until the NEXT
// request arrives -- for a JSON POST that includes the entire "plain" body.
// The Libby relay posts a body and then immediately opens a TLS connection,
// which needs that same memory as contiguous heap, so expose a narrow way to
// hand it back at the point the body is no longer needed.
class CrossPointHttpServer final : public WebServer {
 public:
  explicit CrossPointHttpServer(uint16_t port) : WebServer(port) {}

  void releaseRequestArguments() {
    if (_currentArgs) {
      delete[] _currentArgs;
      _currentArgs = nullptr;
    }
    _currentArgCount = 0;

    if (_postArgs) {
      delete[] _postArgs;
      _postArgs = nullptr;
    }
    _postArgsLen = 0;
  }
};

void releaseRequestArguments(WebServer* server) {
  static_cast<CrossPointHttpServer*>(server)->releaseRequestArguments();
}

// Runs a lambda when the scope ends. Used so an early `return` out of a
// handler still rebuilds the transfer services it suspended.
template <typename Fn>
class ScopedCleanup {
 public:
  explicit ScopedCleanup(Fn fn) : fn_(fn) {}
  ~ScopedCleanup() { fn_(); }
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;

 private:
  Fn fn_;
};
template <typename Fn>
ScopedCleanup(Fn) -> ScopedCleanup<Fn>;

// Where the browser side is allowed to write. Deliberately much narrower than
// "anywhere on the card": the Libby flow only ever persists its credential and
// session under /.crosspoint/, and books plus their rights sidecars under
// /Libby/. A page that has been tampered with therefore cannot overwrite the
// user's library or the firmware's own settings.
bool libbyWritePath(const std::string& p) {
  if (p.find("..") != std::string::npos) return false;
  return p.rfind("/.crosspoint/", 0) == 0 || p.rfind("/Libby/", 0) == 0;
}

// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

// Refresh an EPUB's cache after it changes over the network (upload/replace/delete).
void clearEpubCacheIfNeeded(const String& filePath) {
  if (FsHelpers::hasEpubExtension(filePath)) {
    Epub epub(filePath.c_str(), "/.crosspoint");
    epub.clearCache();  // Drop any stale cache (matters for a replaced or deleted file).
    // If the file is still present (upload/replace, not delete), rebuild the metadata
    // cache now so the Tags/Authors browse views see the new book without waiting for a
    // manual re-cache. skipLoadingCss=true keeps this to a metadata-only parse.
    if (Storage.exists(filePath.c_str())) {
      // A metadata parse can take ~1-2s; bracket it with WDT resets so the fresh
      // watchdog window covers the whole build (the surrounding upload handler is
      // watchdog-subscribed and resets around its SD writes too).
      esp_task_wdt_reset();
      epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true);
      esp_task_wdt_reset();
      LOG_DBG("WEB", "Cached metadata for: %s", filePath.c_str());
    } else {
      LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
    }
  }
  // A book added/removed over the network doesn't change the SD root's FAT mtime,
  // so the library index can't detect it — invalidate it explicitly for any book
  // file so the library re-scans on its next open.
  if (FsHelpers::hasEpubExtension(filePath) || FsHelpers::hasXtcExtension(filePath)) {
    LibraryActivity::invalidateIndexCache();
  }
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (name.equals(HIDDEN_ITEMS[i])) {
      return true;
    }
  }
  return false;
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new CrossPointHttpServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  server->on("/api/clear-library-data", HTTP_POST, [this] { handleClearLibraryData(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // Libby. One page plus the three device capabilities it drives; see the
  // header for why the device side is this thin.
  server->on("/libby", HTTP_GET, [this] { handleLibbyPage(); });
  server->on("/js/libbyCrypto.js", HTTP_GET, [this] { handleLibbyCryptoJs(); });
  server->on("/api/libby/relay", HTTP_POST, [this] { handleLibbyRelay(); });
  server->on("/api/libby/fetch", HTTP_POST, [this] { handleLibbyFetch(); });
  server->on("/api/libby/write", HTTP_POST, [this] { handleLibbyWrite(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d | wsServer=%s (port %d)", port,
            wsServer ? "UP" : "NULL", wsPort);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

bool CrossPointWebServer::scanFiles(const char* path, uint32_t offset, uint32_t limit,
                                    const std::function<void(FileInfo)>& callback) const {
  // Absolute bound on raw entries walked per request. A corrupt FAT chain can
  // make openNextFile() loop forever, and the wdt reset below would keep that
  // alive silently — this cap turns it into a logged, terminating request.
  constexpr uint32_t MAX_RAW_ENTRIES = 10000;

  FsFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return false;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return false;
  }

  LOG_DBG("WEB", "Scanning files in: %s (offset=%u limit=%u)", path, static_cast<unsigned>(offset),
          static_cast<unsigned>(limit));

  FsFile file = root.openNextFile();
  char name[500];
  uint32_t visibleIdx = 0;  // Index over non-hidden entries; offset/limit apply to this.
  uint32_t rawCount = 0;
  bool hasMore = false;
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (fileName.equals(HIDDEN_ITEMS[i])) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      if (visibleIdx >= offset + limit) {
        // One visible entry past the window is enough to know a next page exists.
        hasMore = true;
        file.close();
        break;
      }
      if (visibleIdx >= offset) {
        FileInfo info;
        info.name = fileName;
        info.isDirectory = file.isDirectory();

        if (info.isDirectory) {
          info.size = 0;
          info.isEpub = false;
        } else {
          info.size = file.size();
          info.isEpub = isEpubFile(info.name);
        }

        callback(info);
      }
      ++visibleIdx;
    }

    file.close();
    if (++rawCount >= MAX_RAW_ENTRIES) {
      LOG_ERR("WEB", "Aborting scan of %s after %u raw entries (corrupt directory?)", path,
              static_cast<unsigned>(rawCount));
      break;
    }
    // TEMP diagnostic (web scan hang triage): progress lines make a slow or
    // looping directory walk visible on serial.
    if (rawCount % 25 == 0) {
      LOG_DBG("WEB", "Scan progress: %u entries in %s, last='%s', heap=%u", static_cast<unsigned>(rawCount), path, name,
              static_cast<unsigned>(ESP.getFreeHeap()));
    }
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
  LOG_DBG("WEB", "Scan complete: %u raw entries in %s, hasMore=%d", static_cast<unsigned>(rawCount), path,
          hasMore ? 1 : 0);
  return hasMore;
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");
    // Ensure path starts with /
    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }
    // Remove trailing slash unless it's root
    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  // Pagination: bounds both the SD walk and the streamed response per request,
  // so one huge (or corrupt) directory can't wedge the main loop for minutes.
  constexpr uint32_t DEFAULT_PAGE_SIZE = 100;
  constexpr uint32_t MAX_PAGE_SIZE = 200;
  uint32_t offset = 0;
  uint32_t limit = DEFAULT_PAGE_SIZE;
  if (server->hasArg("offset")) {
    offset = strtoul(server->arg("offset").c_str(), nullptr, 10);
  }
  if (server->hasArg("limit")) {
    limit = strtoul(server->arg("limit").c_str(), nullptr, 10);
    if (limit == 0) limit = DEFAULT_PAGE_SIZE;
    if (limit > MAX_PAGE_SIZE) limit = MAX_PAGE_SIZE;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("{\"items\":[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  const bool hasMore =
      scanFiles(currentPath.c_str(), offset, limit, [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
        doc.clear();
        doc["name"] = info.name;
        doc["size"] = info.size;
        doc["isDirectory"] = info.isDirectory;
        doc["isEpub"] = info.isEpub;

        const size_t written = serializeJson(doc, output, outputSize);
        if (written >= outputSize) {
          // JSON output truncated; skip this entry to avoid sending malformed JSON
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }

        // TEMP diagnostic (web scan hang triage): under heap starvation lwIP can
        // stall each chunk send for seconds, which looks like a hard hang.
        const uint32_t sendStart = millis();
        if (seenFirst) {
          server->sendContent(",");
        } else {
          seenFirst = true;
        }
        server->sendContent(output);
        const uint32_t sendMs = millis() - sendStart;
        if (sendMs > 500) {
          LOG_DBG("WEB", "Slow sendContent: %ums for '%s', heap=%u", static_cast<unsigned>(sendMs), info.name.c_str(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
        }
      });
  if (hasMore) {
    char tail[48];
    snprintf(tail, sizeof(tail), "],\"nextOffset\":%u}", static_cast<unsigned>(offset + limit));
    server->sendContent(tail);
  } else {
    server->sendContent("]}");
  }
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s (offset=%u hasMore=%d)", currentPath.c_str(),
          static_cast<unsigned>(offset), hasMore ? 1 : 0);
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      esp_task_wdt_reset();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    esp_task_wdt_reset();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = server->arg("path");
      // Ensure path starts with /
      if (!state.path.startsWith("/")) {
        state.path = "/" + state.path;
      }
      // Remove trailing slash unless it's root
      if (state.path.length() > 1 && state.path.endsWith("/")) {
        state.path = state.path.substring(0, state.path.length() - 1);
      }
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    // Check if file already exists - SD operations can be slow
    esp_task_wdt_reset();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      esp_task_wdt_reset();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    esp_task_wdt_reset();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearEpubCacheIfNeeded(filePath);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    // Get full file path for post-processing
    String fullPath = state.path;
    if (!fullPath.endsWith("/")) fullPath += "/";
    fullPath += state.fileName;

    // Clear EPUB cache if needed
    clearEpubCacheIfNeeded(fullPath);

    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearEpubCacheIfNeeded(itemPath);
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files are protected
    if (itemName.startsWith(".")) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
      if (itemName.equals(HIDDEN_ITEMS[i])) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      f.close();
      success = Storage.removeDir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearEpubCacheIfNeeded(itemPath);
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  const auto& settings = getSettingsList();

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries
    // Mirror the device UI (SettingsActivity.cpp): entries with STR_NONE_OPT category are
    // legacy/hidden/round-trip-only and must not surface as a bogus "None" group on the web.
    if (s.category == StrId::STR_NONE_OPT) continue;

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        for (const auto& opt : s.enumValues) {
          options.add(I18N.get(opt));
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList();
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        if (val >= 0 && val < static_cast<int>(s.enumValues.size())) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  // Reflect a web-side frontlight change on the hardware immediately.
  halFrontlight.apply(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth);

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

void CrossPointWebServer::handleClearLibraryData() const {
  LOG_DBG("WEB", "Clearing library data (recent books + metadata caches)");

  int clearedCount = 0;
  int failedCount = 0;

  // Delete recent books list
  if (Storage.exists("/.crosspoint/recent.json")) {
    if (Storage.remove("/.crosspoint/recent.json")) {
      clearedCount++;
      LOG_DBG("WEB", "Removed recent.json");
    } else {
      LOG_ERR("WEB", "Failed to remove recent.json");
      failedCount++;
    }
  }

  // Delete all epub_* and xtc_* cache directories (contain chapter/spine/TOC metadata)
  auto root = Storage.open("/.crosspoint");
  if (root && root.isDirectory()) {
    char name[128];
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
      file.getName(name, sizeof(name));
      String itemName(name);
      if (file.isDirectory() && (itemName.startsWith("epub_") || itemName.startsWith("xtc_"))) {
        String fullPath = String("/.crosspoint/") + itemName;
        file.close();
        if (Storage.removeDir(fullPath.c_str())) {
          clearedCount++;
          LOG_DBG("WEB", "Removed cache dir: %s", fullPath.c_str());
        } else {
          LOG_ERR("WEB", "Failed to remove cache dir: %s", fullPath.c_str());
          failedCount++;
        }
      } else {
        file.close();
      }
    }
    root.close();
  } else {
    if (root) root.close();
    LOG_ERR("WEB", "Failed to open /.crosspoint directory");
    server->send(500, "text/plain", "Failed to open library data directory");
    return;
  }

  LOG_DBG("WEB", "Library data cleared: %d removed, %d failed", clearedCount, failedCount);

  if (failedCount == 0) {
    server->send(200, "text/plain", String(clearedCount) + " item(s) cleared");
  } else {
    server->send(500, "text/plain", String(clearedCount) + " cleared, " + String(failedCount) + " failed");
  }
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          // Ensure path is valid
          if (!wsUploadPath.startsWith("/")) wsUploadPath = "/" + wsUploadPath;
          if (wsUploadPath.length() > 1 && wsUploadPath.endsWith("/")) {
            wsUploadPath = wsUploadPath.substring(0, wsUploadPath.length() - 1);
          }

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          esp_task_wdt_reset();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          // Open file for writing
          esp_task_wdt_reset();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          esp_task_wdt_reset();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearEpubCacheIfNeeded(filePath);
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      esp_task_wdt_reset();
      size_t written = wsUploadFile.write(payload, length);
      esp_task_wdt_reset();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearEpubCacheIfNeeded(filePath);

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      FsFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      esp_task_wdt_reset();
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      // Sanitise the uploaded filename (spaces, %, etc. -> '_') rather than rejecting it,
      // so fonts like "lexend 700 12 15_12.cpfont" install as "lexend_700_12_15_12.cpfont".
      char sanitizedName[128];
      if (!FontInstaller::sanitizeCpfontFilename(upload.filename.c_str(), sanitizedName, sizeof(sanitizedName))) {
        LOG_ERR("WEB", "Invalid font filename: %s", upload.filename.c_str());
        break;
      }
      if (strcmp(sanitizedName, upload.filename.c_str()) != 0) {
        LOG_DBG("WEB", "Sanitised font filename: %s -> %s", upload.filename.c_str(), sanitizedName);
      }

      fontUpload.familyName = family.c_str();

      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[128];
      FontInstaller::buildFontPath(family.c_str(), sanitizedName, path, sizeof(path));
      fontUpload.filePath = path;

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", sanitizedName, path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;
      esp_task_wdt_reset();

      if (!fontUpload.magicChecked && upload.currentSize >= 8) {
        if (memcmp(upload.buf, "CPFONT\0\0", 8) != 0) {
          LOG_ERR("WEB", "Invalid .cpfont magic bytes");
          fontUpload.valid = false;
          break;
        }
        fontUpload.magicChecked = true;
      }

      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
          esp_task_wdt_reset();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      fontUpload.file.close();
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}

// ---------------------------------------------------------------------------
// Libby
// ---------------------------------------------------------------------------
//
// The device is a dumb pipe here. Account linking, loan listing, the Adobe
// fulfilment handshake and every crypto primitive it needs run in the browser
// (see src/network/html/LibbyPage.html). What is left are the three things a page
// cannot do for itself: reach a CORS-blocked origin, stream a large body to SD
// without passing it through RAM, and persist a small file.
//
// None of it is resident. These handlers are members of the web server object,
// which exists only inside CrossPointWebServerActivity; that activity reboots
// on the way out, so the feature's RAM cost outside a Libby session is zero.

void CrossPointWebServer::suspendTransferServices() {
  // Leave the WebSocket server alone mid-upload; killing it would abort the
  // transfer. The outbound call just stalls that upload until it completes.
  if (wsServer && !wsUploadInProgress) {
    wsServer->close();
    wsServer.reset();
  }
  if (udpActive) udp.stop();
  LOG_DBG("WEB", "Transfer services suspended, heap %u, max block %u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}

void CrossPointWebServer::resumeTransferServices() {
  if (!running) return;
  if (!wsServer) {
    auto* ws = new (std::nothrow) WebSocketsServer(wsPort);
    if (ws) {
      wsServer.reset(ws);
      wsServer->begin();
      wsServer->onEvent(wsEventCallback);
    } else {
      LOG_ERR("WEB", "OOM: WebSocket server restart");
    }
  }
  if (udpActive) udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Transfer services resumed, heap %u, max block %u", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}

// Parsing a JSON body is memory-expensive on this board and it is worth knowing
// exactly how it failed. WebServer has already made TWO full copies of the body
// by the time a handler runs -- readBytesWithTimeout() mallocs it, then
// `arg.value = String(plainBuf)` copies it again (Parsing.cpp) -- and
// deserializeJson() then builds a third inside the document. So a body that is
// merely large fails as NoMemory, which is a completely different problem from
// malformed JSON and must not be reported as "bad json".
bool CrossPointWebServer::readJsonBody(JsonDocument& out) const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"missing body\"}");
    return false;
  }
  const String& body = server->arg("plain");
  const DeserializationError err = deserializeJson(out, body);
  if (err == DeserializationError::Ok) return true;

  LOG_ERR("WEB", "JSON body rejected: %s (%u bytes, heap %u, max block %u)", err.c_str(), (unsigned)body.length(),
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  char msg[192];
  snprintf(msg, sizeof(msg), "{\"error\":\"%s\",\"bytes\":%u,\"maxBlock\":%u}", err.c_str(), (unsigned)body.length(),
           (unsigned)ESP.getMaxAllocHeap());
  // 413 for the out-of-memory case: it is the payload's size that is the
  // problem, and the caller's remedy is to send it in smaller pieces.
  server->send(err == DeserializationError::NoMemory ? 413 : 400, "application/json", msg);
  return false;
}

void CrossPointWebServer::handleLibbyPage() const {
  sendHtmlContent(server.get(), LibbyPageHtml, sizeof(LibbyPageHtml));
}

// The page's crypto core, served as its own asset rather than inlined: it is
// long enough to be worth reviewing on its own, and keeping it separate means
// the browser caches it across reloads of the page.
void CrossPointWebServer::handleLibbyCryptoJs() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", libbyCryptoJs, libbyCryptoJsCompressedSize);
}

// POST /api/libby/relay {method,url,headers,body}
//   -> {status, headers:[[name,value],...], body}
//
// An authenticated HTTP proxy and nothing more: it ascribes no meaning to any
// header or payload. Headers come back as an ordered, duplicate-preserving list
// so the page can read every Set-Cookie and carry a session itself.
void CrossPointWebServer::handleLibbyRelay() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string url = req["url"] | "";
  const std::string method = req["method"] | "GET";
  if (url.empty()) {
    server->send(400, "application/json", "{\"error\":\"missing url\"}");
    return;
  }

  // Declared before the TLS client so reverse destruction order releases every
  // client/response allocation before these services are rebuilt.
  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
  freeink::SecureHttpClient http;
  http.setUserAgent("CrossPoint");
  // The SecureNet transport ships no CA bundle, so peer verification always
  // fails (wolfSSL -188); skip it like HttpDownloader does. Traffic is still
  // TLS-encrypted, just unauthenticated.
  http.setInsecure();
  if (!http.begin(url)) {
    server->send(502, "application/json", "{\"error\":\"begin failed\"}");
    return;
  }
  if (req["headers"].is<JsonObject>()) {
    for (JsonPair kv : req["headers"].as<JsonObject>()) {
      const char* v = kv.value().as<const char*>();
      http.addHeader(kv.key().c_str(), v ? v : "");
    }
  }
  const std::string body = req["body"] | "";
  // Every value needed below now has independent storage. Drop both copies of
  // the inbound JSON before wolfSSL allocates its handshake working set.
  req.clear();
  req.shrinkToFit();
  releaseRequestArguments(server.get());

  LOG_DBG("WEB", "Libby relay TLS start: heap %u, max block %u: %s", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap(), url.c_str());

  // This task is subscribed to the task WDT for the whole web-server session; a
  // slow peer would otherwise fire it while we block on the response.
  // SecureHttpClient polls shouldAbort in every wait loop, so feed it there.
  const auto feedWatchdog = []() {
    esp_task_wdt_reset();
    return false;  // never aborts; only feeds
  };
  // The body is accumulated once, then streamed out escaped. The copy is
  // hard-capped: an uncapped std::string growth abort()s under -fno-exceptions.
  // Libby's API replies are small JSON; anything big is a book, and books go
  // through /api/libby/fetch, which never buffers.
  static constexpr size_t RELAY_BODY_LIMIT = 32 * 1024;
  std::string respBody;
  bool tooLarge = false;
  bool sized = false;
  const int status = http.sendRequest(
      method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&](const uint8_t* data, size_t len) {
        if (!sized) {
          sized = true;
          if (http.hasContentLength()) {
            const size_t contentLength = http.getContentLength();
            // Known-oversized: refuse before buffering a single chunk. Also bail
            // if the reserve would not fit the largest free block, since the
            // std::string growth that follows would abort() under -fno-exceptions.
            if (contentLength > RELAY_BODY_LIMIT || contentLength + 4096 > ESP.getMaxAllocHeap()) {
              tooLarge = true;
              return false;
            }
            respBody.reserve(contentLength);
          }
        }
        if (respBody.size() + len > RELAY_BODY_LIMIT) {
          tooLarge = true;
          return false;
        }
        respBody.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      feedWatchdog);
  if (tooLarge) {
    LOG_ERR("WEB", "Libby relay response exceeds %u byte cap: %s", (unsigned)RELAY_BODY_LIMIT, url.c_str());
    server->send(413, "application/json", "{\"error\":\"response too large, use fetch\"}");
    return;
  }
  if (status < 0) {
    LOG_ERR("WEB", "Libby relay transport failure: heap %u, max block %u: %s", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(), url.c_str());
    server->send(502, "application/json", "{\"error\":\"transport failure\"}");
    return;
  }
  // A 2xx with an incomplete body would hand the page a silently cut-short
  // payload -- worse than an error, because it parses.
  if (status >= 200 && status < 300 && !http.responseComplete()) {
    LOG_ERR("WEB", "Libby relay truncated: %u bytes (heap %u): %s", (unsigned)respBody.size(),
            (unsigned)ESP.getFreeHeap(), url.c_str());
    server->send(502, "application/json", "{\"error\":\"response truncated\"}");
    return;
  }

  // Serialize only the small header set up front; the body is streamed below so
  // it is never copied into a JsonDocument or a second String.
  JsonDocument headersDoc;
  JsonArray headers = headersDoc.to<JsonArray>();
  for (const auto& h : http.getHeaders()) {
    JsonArray pair = headers.add<JsonArray>();
    pair.add(h.first);
    pair.add(h.second);
  }
  String headersJson;
  serializeJson(headers, headersJson);

  // Stream {"status":N,"headers":[...],"body":"<escaped>"} in chunks so peak RAM
  // is one copy of the body, not three.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "{\"status\":%d,\"headers\":", status);
  server->sendContent(prefix);
  server->sendContent(headersJson);
  server->sendContent(",\"body\":\"");
  std::string chunk;
  chunk.reserve(576);
  for (const char c : respBody) {
    switch (c) {
      case '"':
        chunk += "\\\"";
        break;
      case '\\':
        chunk += "\\\\";
        break;
      case '\b':
        chunk += "\\b";
        break;
      case '\f':
        chunk += "\\f";
        break;
      case '\n':
        chunk += "\\n";
        break;
      case '\r':
        chunk += "\\r";
        break;
      case '\t':
        chunk += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char esc[8];
          snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(c));
          chunk += esc;
        } else {
          chunk += c;  // raw UTF-8 bytes pass through untouched
        }
        break;
    }
    if (chunk.size() >= 512) {
      server->sendContent(chunk.c_str());
      chunk.clear();
      esp_task_wdt_reset();  // each sendContent() is a blocking network write
    }
  }
  if (!chunk.empty()) server->sendContent(chunk.c_str());
  server->sendContent("\"}");
  server->sendContent("");
}

// POST /api/libby/write {path, data, offset}  (data is base64)
//   -> {ok, bytes}
//
// Appends one bounded chunk. offset 0 truncates and creates; a non-zero offset
// must equal the file's current size, so a retried or reordered chunk is
// refused rather than silently corrupting the file.
//
// Chunked rather than whole-file because the body is expensive long before it
// reaches this function: WebServer mallocs the whole thing, copies it into a
// String, and deserializeJson() then builds a third copy in the document
// (see readJsonBody). A one-shot 16KB credential therefore needed ~48KB, much
// of it contiguous, and failed as NoMemory on a board with ~380KB total. The
// cap below keeps that cost flat no matter how large the file is.
void CrossPointWebServer::handleLibbyWrite() {
  // Refuse an oversized body before ArduinoJson tries to parse it -- the point
  // is to avoid the allocation, so checking afterwards would be too late.
  static constexpr size_t MAX_WRITE_CHUNK_B64 = 8 * 1024;
  if (server->hasArg("plain") && server->arg("plain").length() > MAX_WRITE_CHUNK_B64 + 512) {
    LOG_ERR("WEB", "Libby write chunk too large: %u bytes", (unsigned)server->arg("plain").length());
    server->send(413, "application/json", "{\"error\":\"chunk too large\",\"maxChunk\":8192}");
    return;
  }

  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string path = req["path"] | "";
  const std::string dataB64 = req["data"] | "";
  const size_t offset = req["offset"] | 0;
  if (!libbyWritePath(path)) {
    LOG_ERR("WEB", "Rejected Libby write outside allowed roots: '%s'", path.c_str());
    server->send(400, "application/json", "{\"error\":\"bad path\"}");
    return;
  }
  if (dataB64.size() > MAX_WRITE_CHUNK_B64) {
    server->send(413, "application/json", "{\"error\":\"chunk too large\",\"maxChunk\":8192}");
    return;
  }
  std::string decoded;
  decoded.resize((dataB64.size() * 3) / 4 + 3);
  const int32_t n = freeink::content::base64Decode(dataB64.data(), dataB64.size(),
                                                   reinterpret_cast<uint8_t*>(decoded.data()), decoded.size());
  if (n < 0) {
    server->send(400, "application/json", "{\"error\":\"bad base64\"}");
    return;
  }
  decoded.resize(static_cast<size_t>(n));
  req.clear();
  req.shrinkToFit();
  releaseRequestArguments(server.get());

  HalFile f;
  if (offset == 0) {
    // ensureDirectoryExists() creates missing parents, so a first write into a
    // fresh /Libby/ works without anything having made it beforehand.
    const size_t lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      Storage.ensureDirectoryExists(path.substr(0, lastSlash).c_str());
    }
    Storage.remove(path.c_str());
    if (!Storage.openFileForWrite("LIBBY", path, f)) {
      server->send(500, "application/json", "{\"error\":\"cannot write\"}");
      return;
    }
  } else {
    f = Storage.open(path.c_str(), O_RDWR | O_AT_END);
    const size_t existing = f ? f.size() : 0;
    if (!f || existing != offset) {
      if (f) f.close();
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"error\":\"offset mismatch\",\"bytes\":%u}", (unsigned)existing);
      server->send(409, "application/json", msg);
      return;
    }
  }

  const size_t written = decoded.empty() ? 0 : f.write(decoded.data(), decoded.size());
  f.flush();
  const size_t total = f.size();
  f.close();

  JsonDocument resp;
  resp["ok"] = (written == decoded.size());
  resp["bytes"] = total;
  String out;
  serializeJson(resp, out);
  server->send(200, "application/json", out);
}

// POST /api/libby/fetch {url, dest, headers, offset, maxBytes, probe}
//   -> {status, bytes, complete, total}
//
// Streams a URL straight to SD. The book never exists in RAM: bytes go from the
// TLS record to file.write() a chunk at a time, which is the whole reason a
// 5MB EPUB is possible on a board with ~380KB.
//
// Resumable by design. A 2xx only means the headers arrived -- the body can
// still be cut short by a transport drop or a server stall -- so a truncated
// attempt reconnects with a Range request from the byte count already on the
// card. `offset`/`maxBytes` let the page pull a large book as a series of
// bounded segments so no single HTTP request has to survive the whole transfer.
void CrossPointWebServer::handleLibbyFetch() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string url = req["url"] | "";
  const std::string dest = req["dest"] | "";
  const bool probeOnly = req["probe"] | false;
  const size_t requestedOffset = req["offset"] | 0;
  size_t segmentLimit = req["maxBytes"] | 0;
  static constexpr size_t FETCH_MAX_SEGMENT_SIZE = 4 * 1024 * 1024;
  if (segmentLimit > FETCH_MAX_SEGMENT_SIZE) segmentLimit = FETCH_MAX_SEGMENT_SIZE;
  if (url.empty() || (!probeOnly && !libbyWritePath(dest))) {
    server->send(400, "application/json", "{\"error\":\"bad url/dest\"}");
    return;
  }

  std::vector<std::pair<std::string, std::string>> requestHeaders;
  if (req["headers"].is<JsonObject>()) {
    for (JsonPair kv : req["headers"].as<JsonObject>()) {
      const char* value = kv.value().as<const char*>();
      requestHeaders.emplace_back(kv.key().c_str(), value ? value : "");
    }
  }
  req.clear();
  req.shrinkToFit();
  releaseRequestArguments(server.get());

  // A probe answers "how big is this?" without writing anything, so the page can
  // warn before spending the transfer.
  if (probeOnly) {
    suspendTransferServices();
    ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
    freeink::SecureHttpClient probe;
    probe.setInsecure();
    probe.setUserAgent("CrossPoint");
    probe.setFollowRedirects(5);
    for (const auto& header : requestHeaders) probe.addHeader(header.first, header.second);
    if (!probe.begin(url)) {
      server->send(502, "application/json", "{\"error\":\"begin failed\"}");
      return;
    }
    // HEAD, not a ranged GET: every byte a probe pulls is a byte paid twice.
    // Servers that refuse HEAD simply report no length, and the caller falls
    // back to downloading blind rather than being blocked.
    const int status = probe.sendRequest("HEAD", nullptr, 0);
    JsonDocument resp;
    resp["status"] = status;
    if (status >= 200 && status < 400 && probe.hasContentLength()) resp["total"] = probe.getContentLength();
    String out;
    serializeJson(resp, out);
    server->send(200, "application/json", out);
    LOG_DBG("WEB", "Libby fetch probe: status %d: %s", status, url.c_str());
    return;
  }

  HalFile file;
  if (requestedOffset == 0) {
    const size_t lastSlash = dest.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      Storage.ensureDirectoryExists(dest.substr(0, lastSlash).c_str());
    }
    Storage.remove(dest.c_str());
    if (!Storage.openFileForWrite("LIBBY", dest, file)) {
      server->send(500, "application/json", "{\"error\":\"cannot create file\"}");
      return;
    }
  } else {
    file = Storage.open(dest.c_str(), O_RDWR | O_AT_END);
    const size_t existingSize = file ? file.size() : 0;
    if (!file || existingSize != requestedOffset) {
      if (file) file.close();
      char msg[96];
      snprintf(msg, sizeof(msg), "{\"error\":\"offset mismatch\",\"bytes\":%u}", (unsigned)existingSize);
      server->send(409, "application/json", msg);
      return;
    }
  }

  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};

  // A resume keeps all prior progress, so only consecutive zero-progress
  // attempts count against the cap; the absolute ceiling is a backstop against
  // a dead server.
  static constexpr int FETCH_MAX_STALLED_ATTEMPTS = 3;
  static constexpr int FETCH_MAX_TOTAL_ATTEMPTS = 20;
  size_t written = requestedOffset;
  size_t totalExpected = 0;
  size_t nextHeapLog = written;
  bool sdFull = false;
  bool complete = false;
  bool segmentBoundary = false;
  bool rangeUnsupported = false;
  int status = 0;
  int stalled = 0;
  const unsigned long fetchStartedAt = millis();
  unsigned long sdWriteMs = 0;
  unsigned long lastBrowserHeartbeat = fetchStartedAt;
  bool browserResponseStarted = false;

  // A phone may discard an HTTP response that sends no bytes for several minutes
  // even while the device is actively downloading upstream. Start a chunked JSON
  // response once the operation becomes long-running, then send JSON whitespace
  // to keep that browser-facing connection alive.
  const auto keepBrowserAlive = [this, &browserResponseStarted, &lastBrowserHeartbeat]() {
    const unsigned long now = millis();
    if (now - lastBrowserHeartbeat < 5000) return;
    lastBrowserHeartbeat = now;
    if (!server->client().connected()) return;
    if (!browserResponseStarted) {
      server->setContentLength(CONTENT_LENGTH_UNKNOWN);
      server->send(200, "application/json", "");
      browserResponseStarted = true;
    }
    server->sendContent(" \n", 2);
  };
  const auto sendFetchResult = [this, &browserResponseStarted](int code, const String& payload) {
    if (!browserResponseStarted) {
      server->send(code, "application/json", payload);
      return;
    }
    if (server->client().connected()) {
      server->sendContent(payload);
      server->sendContent("", 0);
    }
  };

  for (int attempt = 0; attempt < FETCH_MAX_TOTAL_ATTEMPTS && stalled < FETCH_MAX_STALLED_ATTEMPTS; ++attempt) {
    freeink::SecureHttpClient http;
    http.setUserAgent("CrossPoint");
    http.setInsecure();  // no CA bundle in SecureNet; see handleLibbyRelay()
    // Fulfilment servers can assemble a book on the fly and stall mid-body while
    // packaging; the default 15s no-data timeout truncates those downloads.
    http.setTimeout(60000);
    if (!http.begin(url)) {
      status = -1;
      break;
    }
    for (const auto& header : requestHeaders) http.addHeader(header.first, header.second);
    const bool resuming = written > 0;
    if (resuming) {
      char range[48];
      snprintf(range, sizeof(range), "bytes=%u-", (unsigned)written);
      http.addHeader("Range", range);
      LOG_INF("WEB", "Libby fetch attempt %d resuming from byte %u", attempt + 1, (unsigned)written);
    }
    bool rewindFailed = false;
    bool firstChunk = true;
    size_t attemptStart = written;
    status = http.GET(
        [&](const uint8_t* data, size_t len) {
          esp_task_wdt_reset();
          if (firstChunk) {
            firstChunk = false;
            // Range ignored: this body restarts from byte 0, so the file must too.
            if (resuming && http.getStatus() == 200) {
              if (requestedOffset > 0) {
                rangeUnsupported = true;
                return false;
              }
              file.close();
              if (!Storage.openFileForWrite("LIBBY", dest, file)) {
                rewindFailed = true;
                return false;
              }
              written = 0;
              attemptStart = 0;
            }
          }
          size_t writeLen = len;
          if (segmentLimit > 0) {
            const size_t segmentBytes = written - requestedOffset;
            if (segmentBytes >= segmentLimit) {
              segmentBoundary = true;
              return false;
            }
            writeLen = std::min(writeLen, segmentLimit - segmentBytes);
          }
          // Time the card, not the network: a segment's wall clock is transfer +
          // SD, and only splitting them says whether the link or the card is slow.
          const unsigned long writeStartedAt = millis();
          const size_t wrote = file.write(data, writeLen);
          sdWriteMs += millis() - writeStartedAt;
          if (wrote != writeLen) {
            sdFull = true;
            return false;
          }
          written += writeLen;
          // Heap trajectory during the transfer: a steady value rules RAM out of
          // a mid-body failure; a falling one implicates it.
          if (written >= nextHeapLog) {
            LOG_DBG("WEB", "Libby fetch %u bytes, heap %u", (unsigned)written, (unsigned)ESP.getFreeHeap());
            nextHeapLog = written + 1024 * 1024;
          }
          keepBrowserAlive();
          if (writeLen < len || (segmentLimit > 0 && written - requestedOffset >= segmentLimit)) {
            // Bounded segment requested. Stopping the response callback closes
            // this upstream socket cleanly; the next request resumes with Range.
            segmentBoundary = true;
            return false;
          }
          return true;
        },
        // The data callback only runs when bytes arrive; with the 60s no-data
        // timeout a server stall would starve this task's WDT subscription.
        // shouldAbort is polled in every wait loop.
        [&keepBrowserAlive]() {
          esp_task_wdt_reset();
          keepBrowserAlive();
          return false;  // never aborts; only feeds
        });
    if (sdFull || rewindFailed || rangeUnsupported) break;
    if (status < 200 || status >= 300) break;  // http-level failure: resume cannot help
    // A 206's Content-Length covers only the remainder, so anchor at the
    // attempt's starting offset to get the whole-file size.
    if (totalExpected == 0 && http.hasContentLength()) totalExpected = attemptStart + http.getContentLength();
    if (segmentBoundary) {
      if (totalExpected > 0 && written >= totalExpected) complete = true;
      break;
    }
    if (http.responseComplete()) {
      complete = true;
      break;
    }
    LOG_ERR("WEB", "Libby fetch truncated: %u of %u bytes (heap %u, attempt %d): %s", (unsigned)written,
            (unsigned)totalExpected, (unsigned)ESP.getFreeHeap(), attempt + 1, url.c_str());
    stalled = written > attemptStart ? 0 : stalled + 1;
  }
  file.flush();
  file.close();

  if (segmentBoundary && !complete && status >= 200 && status < 300) {
    JsonDocument resp;
    resp["status"] = status;
    resp["bytes"] = written;
    resp["complete"] = false;
    if (totalExpected > 0) resp["total"] = totalExpected;
    String out;
    serializeJson(resp, out);
    const unsigned long fetchElapsed = millis() - fetchStartedAt;
    LOG_INF("WEB", "Libby fetch segment: %u bytes in %lu ms (%lu%% in SD writes): %s", (unsigned)written, fetchElapsed,
            fetchElapsed > 0 ? (sdWriteMs * 100 / fetchElapsed) : 0, url.c_str());
    sendFetchResult(200, out);
    return;
  }

  if (!complete && status >= 200 && status < 300) {
    Storage.remove(dest.c_str());
    char msg[96];
    const char* error = sdFull ? "sd write failed" : rangeUnsupported ? "range unsupported" : "download truncated";
    // complete:false matters once the heartbeat has committed HTTP 200 chunked:
    // it is the only failure signal the page still sees on this path.
    snprintf(msg, sizeof(msg), "{\"error\":\"%s\",\"bytes\":%u,\"complete\":false}", error, (unsigned)written);
    LOG_ERR("WEB", "Libby fetch failed after %u bytes in %lu ms: %s", (unsigned)written, millis() - fetchStartedAt,
            url.c_str());
    sendFetchResult(502, msg);
    return;
  }

  JsonDocument resp;
  if (status < 200 || status >= 300) {
    Storage.remove(dest.c_str());
    resp["error"] = status < 0 ? "transport failure" : "http status";
  }
  resp["status"] = status;
  resp["bytes"] = written;
  resp["complete"] = complete;
  if (totalExpected > 0) resp["total"] = totalExpected;
  String out;
  serializeJson(resp, out);
  LOG_INF("WEB", "Libby fetch %s: %u bytes in %lu ms: %s", complete ? "complete" : "failed", (unsigned)written,
          millis() - fetchStartedAt, url.c_str());
  sendFetchResult(200, out);
}
