#include "PluginEvents.h"

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "PluginHttp.h"
#include "PluginLocations.h"
#include "TimeUtils.h"
#include "WifiCredentialStore.h"
#include "activities/home/LibraryActivity.h"
#include "network/ProtectedPaths.h"

namespace {

constexpr const char* EVENT_NAMES[] = {"reader.open", "reader.exit", "reader.session", "book.downloaded",
                                       "sleep.enter"};
static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == static_cast<size_t>(pluginevents::Event::COUNT),
              "event name table out of sync");

constexpr const char* OUTBOX_NAME = "/events.jsonl";
// Dropped wholesale past this: by the time 4KB piled up undelivered, the old
// events describe stale state, and a plugin that is never drained must not grow
// a file forever.
constexpr size_t MAX_OUTBOX_BYTES = 4 * 1024;
constexpr size_t MAX_EVENT_LINE = 512;
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
constexpr size_t MAX_EVENT_RESPONSE = 8 * 1024;  // acknowledgements, discarded
constexpr size_t MAX_EVENT_DOWNLOAD = 1024 * 1024;
constexpr size_t MAX_SIDECAR_SIZE = 2 * 1024;
// Sleep-time delivery bounds: never hold sleep for long.
constexpr uint32_t SLEEP_JOIN_MS = 10000;
constexpr uint32_t SLEEP_DRAIN_MS = 10000;
constexpr uint16_t SLEEP_MIN_BATTERY = 20;

// Static subscription table: one slot per plugin with an "events" section,
// sized for a plugin list, not a marketplace. ~1.3KB of .bss on the PSRAM
// boards (the only ones that build this).
constexpr size_t MAX_EVENT_PLUGINS = 8;
constexpr size_t EVENT_COUNT = static_cast<size_t>(pluginevents::Event::COUNT);
struct Subscriber {
  char name[24] = {0};                     // plugin folder name; "" = empty slot
  char dir[64] = {0};                      // "<root>/<name>"
  uint8_t mask = 0;                        // bit per Event
  uint8_t connectMask = 0;                 // events whose handler may bring WiFi up at sleep
  char requireMeta[EVENT_COUNT][24] = {};  // per-event sidecar key filter ("" = none)
};
Subscriber subscribers[MAX_EVENT_PLUGINS];
// Built for this value of SETTINGS.pluginsEnabled; -1 = never built.
int builtForEnabled = -1;
// Any reader event subscribed: read by the render task without the mutex.
std::atomic<bool> readerEventsSubscribed{false};
SemaphoreHandle_t tableMutex = nullptr;

struct TableLock {
  TableLock() {
    if (!tableMutex) tableMutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(tableMutex, portMAX_DELAY);
  }
  ~TableLock() { xSemaphoreGiveRecursive(tableMutex); }
};

uint8_t eventBit(const pluginevents::Event e) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(e)); }

int eventFromName(const char* name) {
  for (size_t i = 0; i < EVENT_COUNT; i++) {
    if (strcmp(EVENT_NAMES[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

std::string outboxPath(const Subscriber& sub) { return std::string(sub.dir) + OUTBOX_NAME; }

// Writes the whole outbox through a temp file, so a power cut mid-write leaves
// either the old queue or the new one, never a torn line.
bool writeOutbox(const std::string& path, const char* bytes, const size_t size) {
  const std::string tmp = path + ".tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("PEVT", tmp, file)) return false;
    const bool written = file.write(reinterpret_cast<const uint8_t*>(bytes), size) == size;
    file.flush();
    file.close();
    if (!written) {
      Storage.remove(tmp.c_str());
      LOG_ERR("PEVT", "Outbox write failed: %s", path.c_str());
      return false;
    }
  }
  Storage.remove(path.c_str());
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    LOG_ERR("PEVT", "Outbox publish failed: %s", path.c_str());
    return false;
  }
  return true;
}

// A book's metadata sidecar (<book>.meta.json) as {meta.KEY} patterns.
void loadMeta(const char* book, pluginhttp::Headers& meta) {
  meta.clear();
  if (!book || !book[0]) return;
  pluginhttp::loadConfigFile(std::string(book) + ".meta.json", meta);
  for (auto& kv : meta) kv.first = "{meta." + kv.first + "}";
}

// require_meta: the sidecar holds `key` as a non-empty string or whole number.
bool sidecarHas(const char* book, const char* key) {
  if (!book || !book[0]) return false;
  std::string raw;
  if (!Storage.readFileToString("PEVT", std::string(book) + ".meta.json", MAX_SIDECAR_SIZE, raw)) return false;
  JsonDocument filter;
  filter[key] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  const JsonVariantConst v = doc[key];
  if (v.is<const char*>()) return v.as<const char*>()[0] != '\0';
  return v.is<long long>() && !v.is<bool>();
}

void ensureTable() {
  if (builtForEnabled != SETTINGS.pluginsEnabled) pluginevents::refreshSubscriptions();
}

}  // namespace

namespace pluginevents {

void refreshSubscriptions() {
  TableLock lock;
  for (auto& sub : subscribers) sub = Subscriber{};
  builtForEnabled = SETTINGS.pluginsEnabled;
  bool readers = false;
  // scanPlugins() is empty while the Plugins setting is off.
  size_t slot = 0;
  for (const auto& entry : PluginLocations::scanPlugins()) {
    if (!entry.hasDevice) continue;
    if (slot >= MAX_EVENT_PLUGINS) {
      LOG_ERR("PEVT", "subscription table full; ignoring %s", entry.name.c_str());
      break;
    }
    // A truncated name or folder would point at another plugin's outbox.
    if (entry.name.size() >= sizeof(Subscriber::name) || entry.dir.size() >= sizeof(Subscriber::dir)) {
      LOG_ERR("PEVT", "plugin name/path too long for events; ignoring %s", entry.name.c_str());
      continue;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", entry.dir + "/device.json", MAX_MANIFEST_SIZE, raw)) continue;
    JsonDocument filter;
    filter["events"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    Subscriber sub;
    for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
      const int e = eventFromName(kv.key().c_str());
      if (e < 0) {
        LOG_DBG("PEVT", "%s: unknown event '%s' ignored", entry.name.c_str(), kv.key().c_str());
        continue;
      }
      const JsonVariantConst handler = kv.value();
      const JsonVariantConst requireMeta = handler["require_meta"];
      if (!requireMeta.isNull()) {
        // A wrong type must not turn the filter off: ignore the handler.
        const char* key = requireMeta.as<const char*>();
        if (!key || !key[0] || strlen(key) >= sizeof(sub.requireMeta[0])) {
          LOG_ERR("PEVT", "%s: %s require_meta invalid; handler ignored", entry.name.c_str(), kv.key().c_str());
          continue;
        }
        strncpy(sub.requireMeta[e], key, sizeof(sub.requireMeta[e]) - 1);
      }
      sub.mask |= static_cast<uint8_t>(1u << e);
      if (handler["connect"] | false) sub.connectMask |= static_cast<uint8_t>(1u << e);
    }
    // sleep.enter exists to act before power-down, so it implies "connect".
    if (sub.mask & eventBit(Event::SleepEnter)) sub.connectMask |= eventBit(Event::SleepEnter);
    if (sub.mask == 0) continue;
    strncpy(sub.name, entry.name.c_str(), sizeof(sub.name) - 1);
    strncpy(sub.dir, entry.dir.c_str(), sizeof(sub.dir) - 1);
    constexpr uint8_t READER_BITS =
        (1u << static_cast<uint8_t>(Event::ReaderOpen)) | (1u << static_cast<uint8_t>(Event::ReaderExit)) |
        (1u << static_cast<uint8_t>(Event::ReaderSession)) | (1u << static_cast<uint8_t>(Event::SleepEnter));
    if (sub.mask & READER_BITS) readers = true;
    LOG_DBG("PEVT", "%s subscribes mask=0x%02x", sub.name, sub.mask);
    subscribers[slot++] = sub;
  }
  readerEventsSubscribed.store(readers);
}

bool anySubscriber(const Event e) {
  if (!SETTINGS.pluginsEnabled) return false;
  TableLock lock;
  ensureTable();
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && (sub.mask & eventBit(e))) return true;
  }
  return false;
}

uint8_t subscriptionMask(const char* plugin) {
  if (!SETTINGS.pluginsEnabled) return 0;
  TableLock lock;
  ensureTable();
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && strcmp(sub.name, plugin) == 0) return sub.mask;
  }
  return 0;
}

void emit(const Event e, const Var* vars, const size_t varCount) {
  if (!anySubscriber(e)) return;
  TableLock lock;

  // {"e":"reader.exit","id":"3fa9c21b-7","ts":1734212345,"vars":{"book":"...","percent":"74"}}
  JsonDocument doc;
  doc["e"] = EVENT_NAMES[static_cast<size_t>(e)];
  // Unique id for server-side dedupe of at-least-once delivery: a per-boot
  // nonce plus a counter, unique across queued events and reboots.
  static const uint32_t bootNonce = esp_random();
  static uint32_t seq = 0;
  char id[24];
  snprintf(id, sizeof(id), "%08lx-%lu", static_cast<unsigned long>(bootNonce), static_cast<unsigned long>(++seq));
  doc["id"] = id;
  // Unix time when the clock is set (RTC / NTP), else 0.
  doc["ts"] = TimeUtils::isClockValid() ? static_cast<long long>(time(nullptr)) : 0LL;
  JsonObject varsObj = doc["vars"].to<JsonObject>();
  const char* book = "";
  for (size_t i = 0; i < varCount; i++) {
    varsObj[vars[i].key] = vars[i].value;
    if (strcmp(vars[i].key, e == Event::BookDownloaded ? "path" : "book") == 0) book = vars[i].value;
  }
  std::string line;
  line.reserve(160);
  serializeJson(doc, line);
  line += '\n';
  if (line.size() > MAX_EVENT_LINE) {
    LOG_ERR("PEVT", "event line too large (%u); dropped", static_cast<unsigned>(line.size()));
    return;
  }

  const size_t ei = static_cast<size_t>(e);
  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || !(sub.mask & eventBit(e))) continue;
    if (sub.requireMeta[ei][0] && !sidecarHas(book, sub.requireMeta[ei])) continue;
    const std::string path = outboxPath(sub);
    std::string queued;
    if (Storage.exists(path.c_str()) &&
        !Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES + MAX_EVENT_LINE, queued)) {
      queued.clear();  // unreadable (or oversized): start over rather than wedge
    }
    if (queued.size() > MAX_OUTBOX_BYTES) queued.clear();
    queued += line;
    writeOutbox(path, queued.data(), queued.size());
  }
}

namespace {

// The manifest subset a drain needs: the handlers plus the token / config /
// password-grant vocabulary shared with the catalog.
struct DrainManifest {
  std::string tokenFile, tokenPath, configFile;
  std::string authType, authTokenPath;
  pluginhttp::RequestSpec authReq;
  struct Handler {
    int event = -1;
    pluginhttp::RequestSpec req;
    std::string dest;  // "download" variant: the response streams here
    bool isDownload() const { return !dest.empty(); }
  };
  std::vector<Handler> handlers;
  bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
};

bool loadDrainManifest(const Subscriber& sub, DrainManifest& out) {
  std::string raw;
  if (!Storage.readFileToString("PEVT", std::string(sub.dir) + "/device.json", MAX_MANIFEST_SIZE, raw)) return false;
  JsonDocument filter;
  filter["token"] = true;
  filter["config"] = true;
  filter["auth"] = true;
  filter["events"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;

  out.tokenFile = pluginhttp::inPluginDir(sub.dir, doc["token"]["file"] | "");
  out.tokenPath = doc["token"]["path"] | "token";
  out.configFile = pluginhttp::inPluginDir(sub.dir, doc["config"]["file"] | "");
  const JsonVariantConst auth = doc["auth"];
  out.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", out.authReq);
  out.authTokenPath = auth["token_path"] | "access_token";

  out.handlers.reserve(EVENT_COUNT);
  for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
    const int e = eventFromName(kv.key().c_str());
    if (e < 0) continue;
    DrainManifest::Handler h;
    h.event = e;
    const JsonVariantConst dl = kv.value()["download"];
    if (dl["url"].as<const char*>()) {
      pluginhttp::readRequest(dl, "GET", h.req);
      h.dest = dl["dest"] | "";
      if (h.dest.empty()) continue;
    } else {
      pluginhttp::readRequest(kv.value()["request"], "POST", h.req);
    }
    if (!h.req.url.empty()) out.handlers.push_back(std::move(h));
  }
  return true;
}

// {token}, {cfg.*}, {meta.*}, {event.ts}, {event.id} and {event.*}. `config`
// and `meta` hold pre-built "{cfg.KEY}" / "{meta.KEY}" patterns.
std::string drainSubstituted(std::string tpl, const std::string& token, const pluginhttp::Headers& config,
                             const pluginhttp::Headers& meta, JsonVariantConst vars, const long long ts,
                             const char* id) {
  pluginhttp::substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  for (const auto& kv : meta) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
  pluginhttp::substituteAll(tpl, "{event.ts}", tsBuf);
  pluginhttp::substituteAll(tpl, "{event.id}", id);
  for (JsonPairConst kv : vars.as<JsonObjectConst>()) {
    pluginhttp::substituteAll(tpl, (std::string("{event.") + kv.key().c_str() + "}").c_str(),
                              pluginhttp::variantToString(kv.value()));
  }
  return tpl;
}

bool expired(const uint32_t deadline) { return deadline && static_cast<int32_t>(millis() - deadline) >= 0; }

uint32_t timeoutFor(const uint32_t deadline) {
  if (!deadline) return 30000;
  const int32_t remaining = static_cast<int32_t>(deadline - millis());
  return remaining > 1000 ? static_cast<uint32_t>(remaining) : 1000;
}

// Replays one queued line. True = delivered (drop it); false = keep for the
// next drain. A corrupt line, or one whose event lost its handler, counts as
// delivered so a manifest edit cannot wedge the queue.
bool deliverLine(const DrainManifest& mf, const std::string& lineText, std::string& token,
                 const pluginhttp::Headers& config, const uint32_t deadline) {
  JsonDocument doc;
  if (deserializeJson(doc, lineText) != DeserializationError::Ok) return true;
  const int e = eventFromName(doc["e"] | "");
  const DrainManifest::Handler* handler = nullptr;
  for (const auto& h : mf.handlers) {
    if (h.event == e) {
      handler = &h;
      break;
    }
  }
  if (!handler) return true;

  const JsonVariantConst vars = doc["vars"];
  const long long ts = doc["ts"] | 0LL;
  const char* id = doc["id"] | "";
  pluginhttp::Headers meta;
  loadMeta(vars[e == static_cast<int>(Event::BookDownloaded) ? "path" : "book"] | "", meta);

  const auto run = [&](const std::string& tok) -> int {
    if (expired(deadline)) return -1;
    pluginhttp::Headers headers;
    headers.reserve(handler->req.headers.size());
    for (const auto& h : handler->req.headers) {
      headers.emplace_back(h.first, drainSubstituted(h.second, tok, config, meta, vars, ts, id));
    }
    const std::string url = drainSubstituted(handler->req.url, tok, config, meta, vars, ts, id);
    const std::string body = drainSubstituted(handler->req.body, tok, config, meta, vars, ts, id);
    if (handler->isDownload()) {
      const std::string dest = drainSubstituted(handler->dest, tok, config, meta, vars, ts, id);
      // Absolute, no "..", never a credential store; retrying cannot fix a bad dest.
      if (dest.empty() || dest[0] != '/' || dest.find("..") != std::string::npos ||
          protectedpaths::isSensitivePath(dest.c_str())) {
        LOG_ERR("PEVT", "unsafe download dest rejected: %s", dest.c_str());
        return 200;
      }
      // Stream to a sibling and swap only on a clean 2xx, so an error body can
      // never replace an existing dest (e.g. /sleep.bmp).
      const std::string tmp = dest + ".part";
      const int st = pluginhttp::requestToFile(nullptr, url, handler->req.method, body, headers, tmp.c_str(),
                                               MAX_EVENT_DOWNLOAD, timeoutFor(deadline));
      if (st >= 200 && st < 300) {
        Storage.remove(dest.c_str());
        if (!Storage.rename(tmp.c_str(), dest.c_str())) return -1;
        const bool hidden = dest.find("/.") != std::string::npos;
        if (!hidden && (FsHelpers::hasEpubExtension(dest) || FsHelpers::hasXtcExtension(dest))) {
          LibraryActivity::invalidateIndexCache();
        }
      } else {
        Storage.remove(tmp.c_str());
      }
      return st;
    }
    std::string response;
    return pluginhttp::request(nullptr, url, handler->req.method, body, headers, response, MAX_EVENT_RESPONSE,
                               timeoutFor(deadline));
  };

  int status = run(token);
  // A password-grant token expires: mint a fresh one and retry once.
  if ((status == 401 || status == 403) && mf.hasPasswordGrant() && !expired(deadline)) {
    pluginhttp::Headers authHeaders;
    authHeaders.reserve(mf.authReq.headers.size());
    for (const auto& h : mf.authReq.headers) {
      authHeaders.emplace_back(h.first, drainSubstituted(h.second, token, config, meta, vars, ts, id));
    }
    std::string minted;
    if (pluginhttp::mintPasswordToken(nullptr, drainSubstituted(mf.authReq.url, token, config, meta, vars, ts, id),
                                      mf.authReq.method,
                                      drainSubstituted(mf.authReq.body, token, config, meta, vars, ts, id), authHeaders,
                                      mf.authTokenPath, minted, timeoutFor(deadline))) {
      pluginhttp::saveTokenToFile(mf.tokenFile, mf.tokenPath, minted);
      token = minted;
      status = run(token);
    }
  }
  return status >= 200 && status < 300;
}

// Ordered: the whole queue, oldest first, stopping at the first failure.
// SleepOnly: only events whose handler opted into "connect" (sleep.enter
// implies it), oldest first; the rest stay queued for an ordinary drain.
enum class DrainMode : uint8_t { Ordered, SleepOnly };

void drainQueued(size_t budget, const DrainMode mode, const uint32_t deadline) {
  if (!SETTINGS.pluginsEnabled) return;
  TableLock lock;
  ensureTable();
  for (const auto& sub : subscribers) {
    if (budget == 0 || expired(deadline)) break;
    if (sub.name[0] == '\0') continue;
    if (mode == DrainMode::SleepOnly && sub.connectMask == 0) continue;
    const std::string path = outboxPath(sub);
    if (!Storage.exists(path.c_str())) continue;
    std::string raw;
    if (!Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES + MAX_EVENT_LINE, raw)) continue;

    DrainManifest mf;
    if (!loadDrainManifest(sub, mf)) continue;  // unreadable right now: keep the queue
    if (mf.handlers.empty()) {
      // Subscribed with no runnable handler: the queue could never advance.
      Storage.remove(path.c_str());
      continue;
    }
    std::string token;
    pluginhttp::Headers config;
    pluginhttp::loadTokenFromFile(mf.tokenFile, mf.tokenPath, token);
    pluginhttp::loadConfigFile(mf.configFile, config);
    for (auto& kv : config) kv.first = "{cfg." + kv.first + "}";

    std::string kept;  // lines left for the next drain
    kept.reserve(raw.size());
    bool stalled = false;
    bool delivered = false;
    for (size_t pos = 0; pos < raw.size();) {
      size_t nl = raw.find('\n', pos);
      if (nl == std::string::npos) nl = raw.size();
      const std::string lineText = raw.substr(pos, nl - pos);
      pos = nl + 1;
      if (lineText.empty()) continue;
      bool eligible = !stalled && budget > 0 && !expired(deadline);
      if (eligible && mode == DrainMode::SleepOnly) {
        JsonDocument filter;
        filter["e"] = true;
        JsonDocument event;
        const int e =
            deserializeJson(event, lineText, DeserializationOption::Filter(filter)) == DeserializationError::Ok
                ? eventFromName(event["e"] | "")
                : -1;
        eligible = e >= 0 && (sub.connectMask & (1u << e));
      }
      if (eligible) {
        --budget;
        if (deliverLine(mf, lineText, token, config, deadline)) {
          delivered = true;
          continue;
        }
        // In order: nothing after a failed event of this plugin may overtake it.
        stalled = true;
      }
      kept += lineText;
      kept += '\n';
    }
    if (kept.empty()) {
      Storage.remove(path.c_str());
    } else if (delivered) {
      writeOutbox(path, kept.data(), kept.size());
    }
    LOG_DBG("PEVT", "%s: drained (stalled=%d)", sub.name, stalled);
  }
}

bool wantsConnectAny() {
  TableLock lock;
  ensureTable();
  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || sub.connectMask == 0) continue;
    std::string raw;
    const std::string path = outboxPath(sub);
    if (!Storage.exists(path.c_str()) ||
        !Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES + MAX_EVENT_LINE, raw)) {
      continue;
    }
    for (size_t i = 0; i < EVENT_COUNT; i++) {
      if (!(sub.connectMask & (1u << i))) continue;
      char field[40];
      snprintf(field, sizeof(field), "\"e\":\"%s\"", EVENT_NAMES[i]);
      if (raw.find(field) != std::string::npos) return true;
    }
  }
  return false;
}

// --- Reader tracking --------------------------------------------------------

// What the render task saw last. Written there, read by the main loop under
// the spinlock (a small scalar copy; no allocation on the render task).
struct ReaderSnapshot {
  uint32_t seq = 0;
  bool onScreen = false;
  ScreenshotInfo::ReaderType type = ScreenshotInfo::ReaderType::None;
  int spine = -1;
  int page = 0;
  int totalPages = 0;
  int percent = 0;
  uint32_t ms = 0;
};
portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
ReaderSnapshot latest;

// Active-reading summary (reader.session), as upstream: only ordinary forward
// page turns count; a page shown 2s or more contributes its time, capped at
// 1800s per page; at least 10s and a set clock are needed to emit.
class ReaderSession {
 public:
  void onPage(const uint32_t nowMs, const int64_t epoch, const int progressBp, const bool forwardTurn) {
    const uint16_t progress = static_cast<uint16_t>(std::min(std::max(progressBp, 0), 10000));
    if (forwardTurn && hasAnchor && anchorEpoch > 0) {
      const uint32_t elapsed = (nowMs - anchorMs) / 1000;
      if (elapsed >= 2) {
        if (duration == 0) {
          startEpoch = anchorEpoch;
          startProgress = anchorProgress;
          endProgress = anchorProgress;
        }
        duration += std::min<uint32_t>(elapsed, 1800);
        endProgress = std::max(endProgress, progress);
      }
    }
    anchorMs = nowMs;
    anchorEpoch = epoch;
    anchorProgress = progress;
    hasAnchor = true;
  }
  void reset() { *this = ReaderSession{}; }
  bool emitWorthy() const { return startEpoch > 0 && duration >= 10; }
  int64_t startTime() const { return startEpoch; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }

 private:
  uint32_t anchorMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  bool hasAnchor = false;
};

// Main-task state.
std::string openBook;  // book a reader.open was queued for ("" = none)
ReaderSnapshot shown;  // the last snapshot handled
uint32_t handledSeq = 0;
ReaderSession session;

void flushSession() {
  if (openBook.empty()) return;
  if (!session.emitWorthy() || !anySubscriber(Event::ReaderSession)) {
    session.reset();
    return;
  }
  // The document hash is computed only when a summary will be sent.
  const std::string document = KOReaderDocumentId::calculate(openBook);
  const bool valid = document.size() == 32 && std::all_of(document.begin(), document.end(), [](const unsigned char c) {
                       return std::isdigit(c) || (c >= 'a' && c <= 'f');
                     });
  if (valid) {
    char startTime[24], endTime[24], duration[16], startProgress[8], endProgress[8];
    snprintf(startTime, sizeof(startTime), "%lld", static_cast<long long>(session.startTime()));
    snprintf(endTime, sizeof(endTime), "%lld",
             static_cast<long long>(session.startTime() + static_cast<int64_t>(session.durationSeconds())));
    snprintf(duration, sizeof(duration), "%lu", static_cast<unsigned long>(session.durationSeconds()));
    snprintf(startProgress, sizeof(startProgress), "%u", session.startProgressBp());
    snprintf(endProgress, sizeof(endProgress), "%u", session.endProgressBp());
    const Var vars[] = {{"book", openBook.c_str()},       {"document", document.c_str()},
                        {"start_time", startTime},        {"end_time", endTime},
                        {"duration_seconds", duration},   {"start_progress_bp", startProgress},
                        {"end_progress_bp", endProgress}, {"progress_scale", "10000"}};
    emit(Event::ReaderSession, vars, 8);
  }
  session.reset();
}

void closeBook() {
  if (openBook.empty()) return;
  flushSession();
  char percent[8];
  snprintf(percent, sizeof(percent), "%d", shown.percent);
  const Var vars[] = {{"book", openBook.c_str()}, {"percent", percent}};
  emit(Event::ReaderExit, vars, 2);
  openBook.clear();
}

// One ordinary forward page turn: the next page of the same chapter, or the
// first page of the next chapter after its last page (EPUB). Reverse turns,
// chapter jumps and re-renders of the same page do not count.
bool isForwardTurn(const ReaderSnapshot& prev, const ReaderSnapshot& now) {
  if (prev.type != now.type) return false;
  if (now.spine == prev.spine) return now.page == prev.page + 1;
  return now.spine == prev.spine + 1 && now.page == 1 && prev.page >= prev.totalPages;
}

}  // namespace

void drain(const size_t maxEvents, const uint32_t budgetMs) {
  if (!SETTINGS.pluginsEnabled || WiFi.status() != WL_CONNECTED) return;
  drainQueued(maxEvents, DrainMode::Ordered, budgetMs ? millis() + budgetMs : 0);
}

bool readerEventsWanted() { return readerEventsSubscribed.load(); }

void noteReaderRender(const ScreenshotInfo& info, const bool onScreen) {
  if (!readerEventsSubscribed.load()) return;
  taskENTER_CRITICAL(&snapshotMux);
  latest.seq++;
  latest.onScreen = onScreen;
  latest.type = info.readerType;
  latest.spine = info.spineIndex;
  latest.page = info.currentPage;
  latest.totalPages = info.totalPages;
  latest.percent = info.progressPercent;
  latest.ms = millis();
  taskEXIT_CRITICAL(&snapshotMux);
}

void pollReader() {
  if (!readerEventsSubscribed.load() && openBook.empty()) return;
  ReaderSnapshot now;
  taskENTER_CRITICAL(&snapshotMux);
  now = latest;
  taskEXIT_CRITICAL(&snapshotMux);
  if (now.seq == handledSeq) return;
  handledSeq = now.seq;

  if (!now.onScreen) {
    closeBook();  // left the reader (Home, another screen)
    shown = now;
    return;
  }
  if (now.type == ScreenshotInfo::ReaderType::None) return;  // a reader child screen
  const std::string& book = APP_STATE.openEpubPath;
  if (book.empty()) return;
  const int64_t epoch = TimeUtils::isClockValid() ? static_cast<int64_t>(time(nullptr)) : 0;
  if (book != openBook) {
    closeBook();
    openBook = book;
    shown = now;
    session.reset();
    session.onPage(now.ms, epoch, now.percent * 100, false);
    const Var vars[] = {{"book", openBook.c_str()}};
    emit(Event::ReaderOpen, vars, 1);
    return;
  }
  // A repaint of the same page (status bar clock, a closed menu) keeps the
  // running dwell instead of restarting it.
  if (now.spine == shown.spine && now.page == shown.page) {
    shown.percent = now.percent;
    return;
  }
  session.onPage(now.ms, epoch, now.percent * 100, isForwardTurn(shown, now));
  shown = now;
}

void prepareSleep(const bool allowNetwork) {
  if (!SETTINGS.pluginsEnabled) return;
  pollReader();  // fold in the page on screen
  flushSession();
  if (!openBook.empty()) {
    char percent[8];
    snprintf(percent, sizeof(percent), "%d", shown.percent);
    const Var vars[] = {{"book", openBook.c_str()}, {"percent", percent}};
    emit(Event::SleepEnter, vars, 2);
  } else {
    emit(Event::SleepEnter, nullptr, 0);
  }

  if (allowNetwork && wantsConnectAny()) {
    bool ownsWifi = false;
    if (WiFi.status() != WL_CONNECTED && WiFi.getMode() == WIFI_OFF &&
        powerManager.getBatteryPercentage() >= SLEEP_MIN_BATTERY) {
      WIFI_STORE.loadFromFile();
      const std::string& ssid = WIFI_STORE.getLastConnectedSsid();
      const WifiCredential* cred = ssid.empty() ? nullptr : WIFI_STORE.findCredential(ssid);
      if (cred) {
        ownsWifi = true;
        WiFi.mode(WIFI_STA);
        if (cred->password.empty()) {
          WiFi.begin(cred->ssid.c_str());
        } else {
          WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
        }
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < SLEEP_JOIN_MS) delay(50);
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      drainQueued(MAX_EVENT_PLUGINS * 2, DrainMode::SleepOnly, millis() + SLEEP_DRAIN_MS);
    } else {
      LOG_INF("PEVT", "sleep delivery skipped: no WiFi");
    }
    if (ownsWifi) {
      WiFi.disconnect(false);
      delay(50);
      WiFi.mode(WIFI_OFF);
    }
  }
  // The reader's own exit is queued after the sleep drain, as upstream: it is
  // delivered on the next online drain.
  closeBook();
}

}  // namespace pluginevents

#endif  // CROSSPOINT_SD_PLUGINS
