#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy

// The web half of the SD-card plugin host (docs/sd-plugins.md): the /plugins
// page, plugin discovery and file serving, the outbound HTTP(S) relay, the
// download-to-SD fetch, small SD writes, and the plugin job queue. Split from
// CrossPointWebServer.cpp so the C3 boards compile none of it.

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_task_wdt.h>
#include <wolfssl/wolfcrypt/coding.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointWebServer.h"
#include "util/PluginLocations.h"

namespace {

// A path component is safe if it has no separators or parent refs.
bool safeComponent(const String& s) {
  return !s.isEmpty() && s.indexOf('/') < 0 && s.indexOf('\\') < 0 && s.indexOf("..") < 0;
}

const char* pluginContentType(const String& file) {
  if (file.endsWith(".js")) return "application/javascript";
  if (file.endsWith(".css")) return "text/css";
  if (file.endsWith(".html")) return "text/html";
  if (file.endsWith(".json")) return "application/json";
  if (file.endsWith(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

}  // namespace

bool CrossPointWebServer::readJsonBody(JsonDocument& out) const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"missing body\"}");
    return false;
  }
  if (deserializeJson(out, server->arg("plain")) != DeserializationError::Ok) {
    server->send(400, "application/json", "{\"error\":\"bad json\"}");
    return false;
  }
  return true;
}

// GET /api/plugins -> [{ "name", "title", "mount" }, ...]. Only plugins with a
// plugin.js are listed (the page loads it); optional manifest.json supplies the
// title and mount point.
void CrossPointWebServer::handlePluginList() const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  for (const auto& e : PluginLocations::scanPlugins()) {
    if (!e.hasPluginJs) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = e.name;
    obj["title"] = e.name;      // overridden by manifest below
    obj["mount"] = "settings";  // default mount point
    std::string manifest;
    if (e.hasManifest && Storage.readFileToString("WEB", e.dir + "/manifest.json", 64 * 1024, manifest)) {
      JsonDocument m;
      if (deserializeJson(m, manifest) == DeserializationError::Ok) {
        if (m["title"].is<const char*>()) obj["title"] = m["title"];
        if (m["mount"].is<const char*>()) obj["mount"] = m["mount"];
      }
    }
  }

  String out;
  serializeJson(doc, out);
  server->send(200, "application/json", out);
}

// GET /plugin?name=<plugin>&file=<file> -> serve /.crosspoint/plugins/<plugin>/<file>
void CrossPointWebServer::handlePluginFile() const {
  const String name = server->arg("name");
  const String file = server->arg("file");
  if (!safeComponent(name) || !safeComponent(file)) {
    server->send(400, "text/plain", "bad plugin path");
    return;
  }
  const std::string pluginDir = PluginLocations::findPluginDir(name.c_str());
  if (pluginDir.empty()) {
    server->send(404, "text/plain", "not found");
    return;
  }
  const std::string path = pluginDir + "/" + file.c_str();
  HalFile f = Storage.open(path.c_str(), O_RDONLY);
  if (!f || !f.isOpen() || f.isDirectory()) {
    if (f) f.close();
    server->send(404, "text/plain", "not found");
    return;
  }

  server->setContentLength(f.size());
  server->send(200, pluginContentType(file), "");
  streamFileToClient(f);
  f.close();
}

// POST /api/relay {plugin, method, url, headers:{}, body} -> {status, body}
// Lets a plugin make an outbound HTTP(S) call the browser can't (CORS): the
// device makes it via SecureNet.
void CrossPointWebServer::handleRelay() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const String plugin = req["plugin"] | "";
  const std::string url = req["url"] | "";
  const std::string method = req["method"] | "GET";
  if (!safeComponent(plugin) || url.empty()) {
    server->send(400, "application/json", "{\"error\":\"missing plugin/url\"}");
    return;
  }

  // Declare the resume guard before the TLS client so reverse destruction
  // releases every client/response allocation before rebuilding these services.
  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
  freeink::SecureHttpClient http;
  http.setUserAgent("CrossPoint");
  // The SecureNet transport ships no CA bundle, so peer verification always
  // fails (wolfSSL -188); skip it like HttpDownloader does.

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
  // All values needed below now have independent storage. Drop both copies of
  // the inbound JSON before wolfSSL allocates its handshake working set.
  req.clear();
  req.shrinkToFit();
  releaseRequestArguments();

  LOG_DBG("WEB", "Relay TLS start: heap %u, max block %u: %s", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap(), url.c_str());

  // This task is subscribed to the task WDT for the whole web-server session;
  // a slow peer would otherwise fire it while we block on the response.
  // SecureHttpClient polls shouldAbort in every wait loop, so feed it there.
  const auto feedWatchdog = [this]() {
    esp_task_wdt_reset();
    return false;  // never aborts; only feeds
  };
  // The response body is accumulated once here, then streamed out escaped
  // (see below). The copy is hard-capped: an uncapped std::string growth
  // abort()s under -fno-exceptions on low heap. Large payloads must use
  // /api/fetch, which streams to SD without buffering.
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
    LOG_ERR("WEB", "Relay response exceeds %u byte cap (url=%s); plugin should use /api/fetch",
            (unsigned)RELAY_BODY_LIMIT, url.c_str());
    server->send(413, "application/json", "{\"error\":\"response too large, use /api/fetch\"}");
    return;
  }
  if (status < 0) {
    LOG_ERR("WEB", "Relay transport failure: heap %u, max block %u: %s", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap(), url.c_str());
    server->send(502, "application/json", "{\"error\":\"transport failure\"}");
    return;
  }
  // Same truncation trap as /api/fetch: a 2xx with an incomplete body would
  // hand the plugin a silently cut-short payload.
  if (status >= 200 && status < 300 && !http.responseComplete()) {
    LOG_ERR("WEB", "Relay truncated: %u bytes (heap %u): %s", (unsigned)respBody.size(), (unsigned)ESP.getFreeHeap(),
            url.c_str());
    server->send(502, "application/json", "{\"error\":\"response truncated\"}");
    return;
  }

  // Serialize only the small header set up front; the body is streamed below so
  // it is never copied into a JsonDocument or a second String. Response headers
  // are order- and duplicate-preserving (so every Set-Cookie is visible), as
  // [name, value] pairs. Generic: the relay is just an authenticated HTTP proxy;
  // it attaches no meaning to any header.
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
  // is one copy of the body, not three. The body is JSON-string escaped on the
  // fly into a small reused buffer flushed every ~512 bytes.
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

namespace {
// A destination path is safe to write if it is absolute and has no parent refs.
bool safeWritePath(const std::string& p) { return p.size() > 1 && p[0] == '/' && p.find("..") == std::string::npos; }

// True when `path` lands inside a plugin folder the native feature set
// supersedes (see PluginLocations::kNativeFeaturePrefixes), under any root —
// the plugin store must not be able to install one around the discovery ban.
bool targetsNativePluginFolder(const std::string& path) {
  for (const char* root : PluginLocations::kRoots) {
    const size_t rootLen = strlen(root);
    if (path.compare(0, rootLen, root) != 0 || path.size() <= rootLen || path[rootLen] != '/') continue;
    const size_t nameStart = rootLen + 1;
    const size_t nameEnd = path.find('/', nameStart);
    const std::string name =
        path.substr(nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart);
    return PluginLocations::isNativeFeature(name.c_str());
  }
  return false;
}

void sendNativeFeatureRefusal(WebServer* server, const char* what) {
  LOG_INF("WEB", "Refused plugin write for '%s': this firmware has that feature built in", what);
  server->send(403, "application/json",
               "{\"error\":\"this feature is built into the firmware; the plugin is not needed\"}");
}
}  // namespace

// POST /api/fetch {plugin, url, dest, headers?, offset?, maxBytes?}
//   -> {status, bytes, complete, total?}
// Device downloads a URL straight to SD, so a large body never passes through
// the browser.
void CrossPointWebServer::handleFetch() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string url = req["url"] | "";
  const std::string dest = req["dest"] | "";
  const bool probeOnly = req["probe"] | false;
  const size_t requestedOffset = req["offset"] | 0;
  size_t segmentLimit = req["maxBytes"] | 0;
  static constexpr size_t FETCH_MAX_SEGMENT_SIZE = 4 * 1024 * 1024;
  if (segmentLimit > FETCH_MAX_SEGMENT_SIZE) segmentLimit = FETCH_MAX_SEGMENT_SIZE;
  if (url.empty() || (!probeOnly && !safeWritePath(dest))) {
    server->send(400, "application/json", "{\"error\":\"bad url/dest\"}");
    return;
  }
  if (!probeOnly && targetsNativePluginFolder(dest)) {
    sendNativeFeatureRefusal(server.get(), dest.c_str());
    return;
  }

  // A probe answers "how big is this?" without writing anything, so a caller can
  // warn before spending the transfer — the size gate BookFusion gets for free
  // from its API, which a plugin fetching an arbitrary URL does not have. dest
  // is not required (and not touched) for one.
  if (probeOnly && url.empty()) {
    server->send(400, "application/json", "{\"error\":\"bad url\"}");
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
  releaseRequestArguments();

  if (probeOnly) {
    suspendTransferServices();
    ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};
    freeink::SecureHttpClient probe;
    probe.setInsecure();
    probe.setUserAgent("CrossPoint");
    probe.setFollowRedirects(5);
    for (const auto& header : requestHeaders) probe.addHeader(header.first, header.second);
    JsonDocument resp;
    if (!probe.begin(url)) {
      server->send(502, "application/json", "{\"error\":\"begin failed\"}");
      return;
    }
    // HEAD, not a ranged GET: every byte a probe pulls is a byte paid twice.
    // Servers that refuse HEAD simply report no length, and the caller falls
    // back to downloading blind rather than being blocked.
    const int status = probe.sendRequest("HEAD", nullptr, 0);
    resp["status"] = status;
    if (status >= 200 && status < 400 && probe.hasContentLength()) {
      resp["total"] = probe.getContentLength();
    }
    String out;
    serializeJson(resp, out);
    server->send(200, "application/json", out);
    LOG_DBG("WEB", "Fetch probe: status %d, total %u: %s", status,
            (unsigned)(probe.hasContentLength() ? probe.getContentLength() : 0), url.c_str());
    return;
  }

  HalFile file;
  if (requestedOffset == 0) {
    // Mirror handlePluginFs(): create missing parents so a plugin's first fetch
    // into a fresh subfolder (e.g. /.crosspoint/plugins/<name>/) doesn't fail
    // before anything has a chance to create it.
    const size_t lastSlash = dest.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      Storage.ensureDirectoryExists(dest.substr(0, lastSlash).c_str());
    }
    Storage.remove(dest.c_str());
    if (!Storage.openFileForWrite("PLG", dest, file)) {
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

  // A 2xx only means the headers arrived; the body can still be cut short by
  // a transport drop, a server stall, or a wolfSSL mid-record OOM. Resume from
  // the received byte count with a Range request on a fresh connection; a
  // server that ignores the Range (200 instead of 206) restarts the body, so
  // the file is rewound before its first chunk lands.
  suspendTransferServices();
  ScopedCleanup resumeServices{[this] { resumeTransferServices(); }};

  // A resume keeps all prior progress, so only consecutive zero-progress
  // attempts count against the cap; the absolute ceiling is a backstop
  // against a dead server.
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
  unsigned long sdWriteMs = 0;  // cumulative time inside file.write(), see the sink below
  unsigned long lastBrowserHeartbeat = fetchStartedAt;
  bool browserResponseStarted = false;

  // A phone may discard an HTTP response that sends no bytes for several
  // minutes even while the device is actively downloading upstream. Start a
  // chunked JSON response only once the operation becomes long-running, then
  // send JSON whitespace to keep that browser-facing connection active.
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
    // The SecureNet transport ships no CA bundle, so peer verification always
    // fails (wolfSSL -188); skip it like HttpDownloader does. Traffic stays
    // TLS-encrypted, just unauthenticated — matching the prior library-lending flow.
    http.setInsecure();
    // Some delivery servers assemble books on the fly and can stall mid-body
    // while packaging; the default 15s no-data timeout truncates those downloads.
    http.setTimeout(60000);
    if (!http.begin(url)) {
      status = -1;
      break;
    }
    for (const auto& header : requestHeaders) {
      http.addHeader(header.first, header.second);
    }
    const bool resuming = written > 0;
    if (resuming) {
      char range[48];
      snprintf(range, sizeof(range), "bytes=%u-", (unsigned)written);
      http.addHeader("Range", range);
      LOG_INF("WEB", "Fetch attempt %d resuming from byte %u", attempt + 1, (unsigned)written);
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
              if (!Storage.openFileForWrite("PLG", dest, file)) {
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
          // SD, and only splitting them says whether coalescing these per-chunk
          // writes (each one carries the HalStorage mutex and SdFat bookkeeping)
          // would buy anything, or whether the link is simply slow.
          const unsigned long writeStartedAt = millis();
          const size_t wrote = file.write(data, writeLen);
          sdWriteMs += millis() - writeStartedAt;
          if (wrote != writeLen) {
            sdFull = true;
            return false;
          }
          written += writeLen;
          // Heap trajectory during the transfer: a steady value rules RAM out of a
          // mid-body failure; a falling one implicates it.
          if (written >= nextHeapLog) {
            LOG_DBG("WEB", "Fetch %u bytes, heap %u", (unsigned)written, (unsigned)ESP.getFreeHeap());
            nextHeapLog = written + 1024 * 1024;
          }
          keepBrowserAlive();
          if (writeLen < len || (segmentLimit > 0 && written - requestedOffset >= segmentLimit)) {
            // The caller requested a bounded segment. Stopping the response
            // callback closes this upstream socket cleanly; the next browser
            // request resumes from `written` with Range.
            segmentBoundary = true;
            return false;
          }
          return true;
        },
        // The data callback only runs when bytes arrive; with the 60s
        // no-data timeout a server stall would starve this task's WDT
        // subscription. shouldAbort is polled in every wait loop.
        [this, &keepBrowserAlive]() {
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
    LOG_ERR("WEB", "Fetch truncated: %u of %u bytes (heap %u, attempt %d): %s", (unsigned)written,
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
    LOG_INF("WEB", "Fetch timing: %lu ms total, %lu ms in SD writes (%lu%%), %u KB/s overall", fetchElapsed, sdWriteMs,
            fetchElapsed > 0 ? (sdWriteMs * 100 / fetchElapsed) : 0,
            fetchElapsed > 0 ? (unsigned)((written >> 10) * 1000UL / fetchElapsed) : 0);
    LOG_INF("WEB", "Fetch segment complete: %u bytes total in %lu ms: %s", (unsigned)written, fetchElapsed,
            url.c_str());
    sendFetchResult(200, out);
    return;
  }

  if (!complete && status >= 200 && status < 300) {
    Storage.remove(dest.c_str());
    char msg[96];
    const char* error = sdFull ? "sd write failed" : rangeUnsupported ? "range unsupported" : "download truncated";
    // complete:false matters once the heartbeat has committed HTTP 200 chunked:
    // it is the only signal fetchToSd()'s resume loop still sees on this path
    // (it then detects zero progress and throws instead of returning success).
    snprintf(msg, sizeof(msg), "{\"error\":\"%s\",\"bytes\":%u,\"complete\":false}", error, (unsigned)written);
    LOG_ERR("WEB", "Fetch failed after %u bytes in %lu ms: %s", (unsigned)written, millis() - fetchStartedAt,
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
  const bool browserConnected = server->client().connected();
  LOG_INF("WEB", "Fetch %s: %u bytes in %lu ms, browser %s: %s", complete ? "complete" : "failed", (unsigned)written,
          millis() - fetchStartedAt, browserConnected ? "connected" : "disconnected", url.c_str());
  sendFetchResult(200, out);
}

// POST /api/plugin-fs?plugin=<name>&path=<path> with the raw file contents as
// the request body. A plugin writes a small file to SD.
void CrossPointWebServer::handlePluginFs() {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }
  const String plugin = server->arg("plugin");
  const std::string path = server->arg("path").c_str();
  if (!safeComponent(plugin) || !safeWritePath(path)) {
    LOG_ERR("WEB", "Rejected plugin file write: plugin='%s' path='%s'", plugin.c_str(), path.c_str());
    server->send(400, "application/json", "{\"error\":\"bad path\"}");
    return;
  }
  if (PluginLocations::isNativeFeature(plugin.c_str()) || targetsNativePluginFolder(path)) {
    sendNativeFeatureRefusal(server.get(), plugin.c_str());
    return;
  }

  // Resolve the body before touching the filesystem. PluginHost sends the file
  // base64-encoded (enc=base64) so a binary payload with NUL bytes survives the
  // WebServer's String parsing, which otherwise truncates at the first NUL. A
  // legacy raw body (no enc) is written verbatim.
  const String& rawData = server->arg("plain");
  std::string decoded;
  const uint8_t* data = reinterpret_cast<const uint8_t*>(rawData.c_str());
  size_t dataSize = rawData.length();
  if (server->arg("enc") == "base64") {
    const size_t encLen = rawData.length();
    // Plugin files (config, token, rights, credential) are small; the cap also
    // bounds the fallible resize below under -fno-exceptions.
    static constexpr size_t kMaxPluginFile = 256 * 1024;
    if (encLen > kMaxPluginFile) {
      server->send(413, "application/json", "{\"error\":\"too large\"}");
      return;
    }
    decoded.resize((encLen * 3) / 4 + 3);
    word32 dn = decoded.size();
    if (Base64_Decode(reinterpret_cast<const byte*>(rawData.c_str()), static_cast<word32>(encLen),
                      reinterpret_cast<byte*>(decoded.data()), &dn) != 0) {
      server->send(400, "application/json", "{\"error\":\"bad base64\"}");
      return;
    }
    decoded.resize(static_cast<size_t>(dn));
    data = reinterpret_cast<const uint8_t*>(decoded.data());
    dataSize = decoded.size();
  }

  // ensureDirectoryExists() creates missing parents along the way, so this
  // covers any depth under /.crosspoint/plugins/<name>/... in one call.
  const size_t lastSlash = path.rfind('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    Storage.ensureDirectoryExists(path.substr(0, lastSlash).c_str());
  }
  HalFile f;
  if (!Storage.openFileForWrite("PLG", path, f)) {
    server->send(500, "application/json", "{\"error\":\"cannot write\"}");
    return;
  }
  const size_t n = dataSize == 0 ? 0 : f.write(data, dataSize);
  f.flush();
  f.close();

  JsonDocument resp;
  resp["ok"] = (n == dataSize);
  resp["bytes"] = n;
  String out;
  serializeJson(resp, out);
  server->send(200, "application/json", out);
}

CrossPointWebServer::PluginJob* CrossPointWebServer::allocPluginJob() {
  PluginJob* best = nullptr;
  for (auto& job : pluginJobs) {
    if (job.state == JOB_EMPTY) return &job;
    const bool finished = job.state == JOB_DONE || job.state == JOB_ERROR;
    if (finished && (!best || job.updatedAt < best->updatedAt)) best = &job;
  }
  return best;
}

// POST /api/plugin-jobs {plugin, action, args?} -> {id}
void CrossPointWebServer::handlePluginJobSubmit() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const String plugin = req["plugin"] | "";
  const String action = req["action"] | "";
  std::string args;
  if (!req["args"].isNull()) serializeJson(req["args"], args);
  // The claim response embeds `action` in a snprintf-built JSON template, so
  // it must be identifier-safe; anything needing escaping is rejected here.
  const auto identifierSafe = [](const String& s) {
    for (const char c : s) {
      if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    }
    return !s.isEmpty();
  };
  if (!safeComponent(plugin) || !identifierSafe(action) || plugin.length() >= sizeof(PluginJob::plugin) ||
      action.length() >= sizeof(PluginJob::action) || args.size() >= sizeof(PluginJob::args)) {
    server->send(400, "application/json", "{\"error\":\"bad plugin/action/args\"}");
    return;
  }
  PluginJob* job = allocPluginJob();
  if (!job) {
    server->send(503, "application/json", "{\"error\":\"job queue full\"}");
    return;
  }
  *job = PluginJob{};
  job->id = nextPluginJobId++;
  job->state = JOB_PENDING;
  job->updatedAt = millis();
  snprintf(job->plugin, sizeof(job->plugin), "%s", plugin.c_str());
  snprintf(job->action, sizeof(job->action), "%s", action.c_str());
  snprintf(job->args, sizeof(job->args), "%s", args.c_str());
  LOG_INF("WEB", "Plugin job %u queued: %s/%s", (unsigned)job->id, job->plugin, job->action);
  char msg[48];
  snprintf(msg, sizeof(msg), "{\"id\":%u}", (unsigned)job->id);
  server->send(200, "application/json", msg);
}

// GET /api/plugin-jobs/claim?plugin=<name> -> {id, action, args} or {id:0}
void CrossPointWebServer::handlePluginJobClaim() {
  const String plugin = server->arg("plugin");
  const uint32_t now = millis();
  for (auto& job : pluginJobs) {
    if (job.state == JOB_RUNNING && now - job.updatedAt > PLUGIN_JOB_LEASE_MS) {
      job.state = JOB_PENDING;
      job.updatedAt = now;
      LOG_INF("WEB", "Plugin job %u lease expired; requeued", (unsigned)job.id);
    }
    if (job.state != JOB_PENDING || plugin != job.plugin) continue;
    job.state = JOB_RUNNING;
    job.updatedAt = now;
    const std::string msg = "{\"id\":" + std::to_string(job.id) + ",\"action\":\"" + job.action +
                            "\",\"args\":" + (job.args[0] ? job.args : "{}") + "}";
    server->send(200, "application/json", msg.c_str());
    return;
  }
  server->send(200, "application/json", "{\"id\":0}");
}

// POST /api/plugin-jobs/complete {id, ok, result?} -> {ok}
void CrossPointWebServer::handlePluginJobComplete() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const uint32_t id = req["id"] | 0;
  for (auto& job : pluginJobs) {
    if (job.id != id) continue;
    if (job.state == JOB_DONE || job.state == JOB_ERROR) {
      server->send(200, "application/json", "{\"ok\":true}");
      return;
    }
    if (job.state != JOB_RUNNING) break;
    job.state = (req["ok"] | false) ? JOB_DONE : JOB_ERROR;
    job.updatedAt = millis();
    std::string result;
    if (!req["result"].isNull()) serializeJson(req["result"], result);
    if (result.size() >= sizeof(job.result)) result = "{\"error\":\"result too large\"}";
    snprintf(job.result, sizeof(job.result), "%s", result.c_str());
    LOG_INF("WEB", "Plugin job %u %s", (unsigned)id, job.state == JOB_DONE ? "done" : "failed");
    server->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  server->send(404, "application/json", "{\"error\":\"no such running job\"}");
}

// GET /api/plugin-jobs/status?id=<n> -> {id, state, result}
void CrossPointWebServer::handlePluginJobStatus() {
  const uint32_t id = strtoul(server->arg("id").c_str(), nullptr, 10);
  static constexpr const char* kStateNames[] = {"empty", "pending", "running", "done", "error"};
  for (auto& job : pluginJobs) {
    if (job.id != id || job.state == JOB_EMPTY) continue;
    const std::string msg = "{\"id\":" + std::to_string(id) + ",\"state\":\"" + kStateNames[job.state] +
                            "\",\"result\":" + (job.result[0] ? job.result : "null") + "}";
    server->send(200, "application/json", msg.c_str());
    return;
  }
  // Unknown: never existed, or its slot was recycled after completion.
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"id\":%u,\"state\":\"unknown\",\"result\":null}", (unsigned)id);
  server->send(200, "application/json", msg);
}

#endif  // CROSSPOINT_SD_PLUGINS
