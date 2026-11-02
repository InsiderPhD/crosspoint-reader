#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PluginCatalogActivity.h"
#include "PluginInfoActivity.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/ButtonNavigator.h"
#include "util/FirmwareIdentity.h"
#include "util/HtmlToPlainText.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#include "util/QrUtils.h"
#include "util/StringUtils.h"
#include "util/TouchListNav.h"
#include "util/UrlUtils.h"

// Template/JSON/transport primitives shared with the plugin event drain.
using pluginhttp::readHeaders;
using pluginhttp::resolvePath;
using pluginhttp::segIsIndex;
using pluginhttp::splitPath;
using pluginhttp::substituteAll;
using pluginhttp::urlEncodeQuery;
using pluginhttp::variantToString;

namespace {
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
// In-DRAM responses (auth, download-url hops) are small; the cap bounds a
// misbehaving server, not normal use.
constexpr size_t MAX_API_RESPONSE = 48 * 1024;
// Browse responses stream to this SD temp file instead of DRAM: one page of
// raw catalog JSON can run 60+ KB (BookFusion inlines heavy per-book
// metadata), and buffering that in a std::string aborts on low heap.
constexpr char BROWSE_TMP_PATH[] = "/.pcat_tmp.json";
constexpr size_t MAX_BROWSE_RESPONSE = 1024 * 1024;
// browse.lists_url answers are small JSON indexes held in DRAM (PSRAM on the
// boards that build plugins); the list count bounds the vector, since a 16KB
// index of tiny entries would otherwise reserve hundreds of entries.
constexpr size_t MAX_BROWSE_LIST_INDEX_RESPONSE = 16 * 1024;
constexpr size_t MAX_BROWSE_LISTS = 64;
// A details_url page and a row's description are bounded: they are read for
// one item when its Details screen opens, then freed with that screen.
constexpr size_t MAX_ITEM_DETAILS_RESPONSE = 24 * 1024;
constexpr size_t MAX_ITEM_DESCRIPTION = 8 * 1024;
constexpr int MAX_PAGE_SIZE = 16;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long PLUGIN_UNINSTALL_HOLD_MS = 1000;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Builds an ArduinoJson deserialization filter keeping only one dotted path.
// A numeric segment becomes filter index [0], which ArduinoJson applies to
// every array element. Filtering keeps the parsed document to a few KB where
// the unfiltered catalog response would cost several times the body size.
void addFilterPath(JsonDocument& filter, const std::vector<std::string>& segs) {
  JsonVariant node = filter.as<JsonVariant>();
  for (size_t i = 0; i < segs.size(); i++) {
    const bool last = i + 1 == segs.size();
    if (segIsIndex(segs[i])) {
      JsonArray arr = node.is<JsonArray>() ? node.as<JsonArray>() : node.to<JsonArray>();
      if (last) {
        if (arr.size() == 0) arr.add(true);
        return;
      }
      const bool nextIndex = segIsIndex(segs[i + 1]);
      if (arr.size() == 0) {
        if (nextIndex)
          arr.add<JsonArray>();
        else
          arr.add<JsonObject>();
      }
      node = arr[0];
    } else {
      JsonObject obj = node.is<JsonObject>() ? node.as<JsonObject>() : node.to<JsonObject>();
      if (last) {
        obj[segs[i]] = true;
        return;
      }
      const bool nextIndex = segIsIndex(segs[i + 1]);
      JsonVariant child = obj[segs[i]];
      if (child.isNull()) {
        if (nextIndex)
          child = obj[segs[i]].to<JsonArray>();
        else
          child = obj[segs[i]].to<JsonObject>();
      }
      node = child;
    }
  }
}

void addFieldFilter(JsonDocument& filter, const std::string& itemsPath, const std::string& fieldPath) {
  if (fieldPath.empty()) return;
  std::vector<std::string> segs;
  splitPath(itemsPath, segs);
  segs.push_back("0");  // the item array: index filter applies to all elements
  std::vector<std::string> fieldSegs;
  splitPath(fieldPath, fieldSegs);
  segs.insert(segs.end(), fieldSegs.begin(), fieldSegs.end());
  addFilterPath(filter, segs);
}

std::string md5Hex(const std::string& text) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  md5.calculate();
  return md5.toString().c_str();
}

// Returns `override` unless it's empty, in which case `fallback` applies.
const std::string& pick(const std::string& override, const std::string& fallback) {
  return override.empty() ? fallback : override;
}

// --- Catalog compatibility (upstream CrossPoint 2.0 catalog.compatibility /
// catalog.list_requirements) ---------------------------------------------
std::string_view trimCsvToken(std::string_view token) {
  while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
  while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
  return token;
}

// Calls fn(token) for each trimmed comma-separated token; stops early (and
// returns false) when fn returns false.
template <typename Fn>
bool allCsvTokens(const std::string_view csv, Fn fn) {
  size_t start = 0;
  while (true) {
    size_t end = csv.find(',', start);
    if (end == std::string_view::npos) end = csv.size();
    if (!fn(trimCsvToken(csv.substr(start, end - start)))) return false;
    if (end == csv.size()) return true;
    start = end + 1;
  }
}

bool csvContainsToken(const std::string_view csv, const std::string_view wanted) {
  return !allCsvTokens(csv, [wanted](const std::string_view token) { return token != wanted; });
}

bool hasCapability(const std::string_view capability) {
  return !capability.empty() && csvContainsToken(firmwareidentity::PLUGIN_CAPABILITIES, capability);
}

// An entry is compatible unless it names a family list without ours, a
// minimum plugin API above ours, or a capability we do not advertise.
bool catalogEntryCompatible(const std::string& family, const std::string& apiMin, const std::string& required) {
  if (!family.empty() && !csvContainsToken(family, firmwareidentity::FAMILY)) return false;
  if (!apiMin.empty()) {
    char* end = nullptr;
    const long parsed = strtol(apiMin.c_str(), &end, 10);
    if (end == apiMin.c_str() || *end != '\0' || parsed < 0 || parsed > firmwareidentity::PLUGIN_API_VERSION)
      return false;
  }
  return required.empty() || allCsvTokens(required, hasCapability);
}

// A catalog's `requires` field as one comma-separated string: either a string
// already, or an array of names (joined).
std::string csvField(const JsonVariantConst v) {
  if (!v.is<JsonArrayConst>()) return variantToString(v);
  std::string out;
  for (JsonVariantConst name : v.as<JsonArrayConst>()) {
    const char* text = name.as<const char*>();
    if (!text) continue;
    if (!out.empty()) out += ',';
    out += text;
  }
  return out;
}

// A list's optional `requires` is an all-of array of capability names.
// Missing means public; a malformed or oversized array never exposes a list.
bool matchesListRequirements(const JsonVariantConst requirements) {
  if (requirements.isUnbound() || requirements.isNull()) return true;
  JsonArrayConst required = requirements.as<JsonArrayConst>();
  if (required.isNull() || required.size() > 32) return false;
  for (JsonVariantConst v : required) {
    const char* capability = v.as<const char*>();
    if (!capability || !hasCapability(capability)) return false;
  }
  return true;
}

// Reads a list's body / notice, shared by manifest lists and fetched ones.
void readListExtras(const JsonVariantConst l, std::string& body, std::string& noticeTitle, std::string& noticeMessage,
                    std::string& noticeConfirm, std::string& noticeCancel) {
  body = l["body"] | "";
  const JsonVariantConst notice = l["notice"];
  noticeTitle = notice["title"] | "";
  noticeMessage = notice["message"] | "";
  noticeConfirm = notice["confirm"] | "";
  noticeCancel = notice["cancel"] | "";
}

// --- XML-list helpers -----------------------------------------------------
std::string urlDecode(const std::string& s) {
  std::string plusesAsSpaces = s;
  std::replace(plusesAsSpaces.begin(), plusesAsSpaces.end(), '+', ' ');
  return FsHelpers::decodeUriEscapes(plusesAsSpaces);
}

std::string urlEncodePath(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (const unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~/", c)) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// scheme://host[:port] of a URL, for turning server-absolute hrefs into full URLs.
std::string originOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t hostEnd = url.find('/', schemeEnd + 3);
  return hostEnd == std::string::npos ? url : url.substr(0, hostEnd);
}

std::string pathOf(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return url;
  const size_t hostEnd = url.find('/', schemeEnd + 3);
  return hostEnd == std::string::npos ? "/" : url.substr(hostEnd);
}

std::string basename(const std::string& path) {
  std::string p = path;
  while (!p.empty() && p.back() == '/') p.pop_back();
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

// True when `path` ends in one of the allowed extensions (case-insensitive).
// An empty list allows everything.
bool hasAllowedExtension(const std::string& path, const std::vector<std::string>& exts) {
  if (exts.empty()) return true;
  std::string lower = path;
  for (auto& c : lower) c = tolower(static_cast<unsigned char>(c));
  while (!lower.empty() && lower.back() == '/') lower.pop_back();
  for (const auto& ext : exts) {
    if (lower.size() >= ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) return true;
  }
  return false;
}

// Extracts one row per repeating item element from an XML list via expat (the
// parser the OPDS browser already uses), instead of scanning tags by hand.
// Elements match on local name (namespace prefix stripped). Selector forms:
// "elem" (leading text of the first matching descendant), "elem@attr"
// (attribute of the first matching descendant), "@attr" (attribute on the
// item element itself).
class XmlListParser {
 public:
  enum Field { F_URL, F_TITLE, F_AUTHOR, F_ID, F_COUNT };
  struct RawItem {
    std::string field[F_COUNT];
    bool isDir = false;
  };

  XmlListParser(const std::string& itemName, const std::string& containerName, const std::string (&selectors)[F_COUNT])
      : item(itemName), container(containerName) {
    for (int i = 0; i < F_COUNT; i++) splitSelector(selectors[i], sel[i]);
  }

  // Parses the whole document from an SD file in small chunks, so the raw XML
  // (a large WebDAV multistatus, ...) never occupies DRAM. Rows collected
  // before a parse error are kept, mirroring the previous scanner, which
  // stopped at the first bad tag.
  std::vector<RawItem> parseFile(HalFile& f) {
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) return std::move(rows);
    XML_SetUserData(p, this);
    XML_SetElementHandler(
        p,
        [](void* self, const XML_Char* name, const XML_Char** atts) {
          static_cast<XmlListParser*>(self)->onStart(name, atts);
        },
        [](void* self, const XML_Char* name) { static_cast<XmlListParser*>(self)->onEnd(name); });
    XML_SetCharacterDataHandler(
        p, [](void* self, const XML_Char* s, int len) { static_cast<XmlListParser*>(self)->onText(s, len); });
    std::vector<char> buf(2048);
    for (;;) {
      const int n = f.read(buf.data(), buf.size());
      const bool last = n <= 0;
      if (XML_Parse(p, buf.data(), last ? 0 : n, last ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
        LOG_ERR("PCAT", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(p),
                XML_ErrorString(XML_GetErrorCode(p)));
        break;
      }
      if (last) break;
    }
    destroyXmlParser(p);
    return std::move(rows);
  }

 private:
  static constexpr size_t MAX_ITEMS = 200;
  static constexpr size_t MAX_FIELD_CHARS = 768;

  struct Selector {
    std::string elem, attr;
    bool onItemTag = false;  // "@attr": read from the item element's own tag
    bool isSet() const { return !elem.empty() || !attr.empty(); }
  };

  static void splitSelector(const std::string& s, Selector& out) {
    if (s.empty()) return;
    if (s[0] == '@') {
      out.onItemTag = true;
      out.attr = s.substr(1);
      return;
    }
    const size_t at = s.find('@');
    out.elem = at == std::string::npos ? s : s.substr(0, at);
    if (at != std::string::npos) out.attr = s.substr(at + 1);
  }

  static const char* localName(const XML_Char* name) {
    const char* colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
  }

  static const char* findAttr(const XML_Char** atts, const std::string& attr) {
    for (int i = 0; atts[i]; i += 2) {
      if (attr == atts[i]) return atts[i + 1];
    }
    return nullptr;
  }

  void onStart(const XML_Char* name, const XML_Char** atts) {
    depth++;
    const char* local = localName(name);
    if (itemDepth < 0) {
      if (rows.size() < MAX_ITEMS && item == local) {
        itemDepth = depth;
        current = RawItem{};
        capturingMask = 0;
        for (int i = 0; i < F_COUNT; i++) {
          done[i] = !sel[i].isSet();
          if (sel[i].onItemTag) {
            const char* v = findAttr(atts, sel[i].attr);
            if (v) current.field[i] = v;
            done[i] = true;
          }
        }
      }
      return;
    }
    // Inside an item: a child element ends any leading-text capture.
    capturingMask = 0;
    if (!container.empty() && container == local) current.isDir = true;
    for (int i = 0; i < F_COUNT; i++) {
      if (done[i] || sel[i].onItemTag || sel[i].elem != local) continue;
      done[i] = true;  // first matching descendant wins
      if (!sel[i].attr.empty()) {
        const char* v = findAttr(atts, sel[i].attr);
        if (v) current.field[i] = v;
      } else {
        capturingMask |= 1u << i;
      }
    }
  }

  void onEnd(const XML_Char* name) {
    if (itemDepth >= 0) {
      capturingMask = 0;
      if (depth == itemDepth && item == localName(name)) {
        for (auto& f : current.field) trim(f);
        rows.push_back(std::move(current));
        itemDepth = -1;
      }
    }
    depth--;
  }

  void onText(const XML_Char* s, const int len) {
    if (itemDepth < 0 || capturingMask == 0) return;
    for (int i = 0; i < F_COUNT; i++) {
      if (!(capturingMask & (1u << i)) || current.field[i].size() >= MAX_FIELD_CHARS) continue;
      current.field[i].append(s, std::min<size_t>(len, MAX_FIELD_CHARS - current.field[i].size()));
    }
  }

  static void trim(std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
      s.clear();
      return;
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
  }

  std::string item;
  std::string container;
  Selector sel[F_COUNT];
  std::vector<RawItem> rows;
  RawItem current;
  bool done[F_COUNT] = {};
  uint8_t capturingMask = 0;
  int depth = 0;
  int itemDepth = -1;
};
}  // namespace

namespace {
// Reads "title"/"description" from a plugin JSON file into the ref, only
// overwriting non-empty values (so device.json wins over manifest.json).
void readTitleDesc(const std::string& path, PluginRef& ref) {
  std::string raw;
  if (!Storage.readFileToString("PCAT", path, MAX_MANIFEST_SIZE, raw)) return;
  JsonDocument filter;
  filter["title"] = true;
  filter["description"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return;
  if (doc["title"].is<const char*>()) ref.title = doc["title"].as<const char*>();
  if (doc["description"].is<const char*>()) ref.description = doc["description"].as<const char*>();
}
}  // namespace

std::vector<PluginRef> discoverPlugins() {
  const auto entries = PluginLocations::scanPlugins();
  std::vector<PluginRef> plugins;
  plugins.reserve(entries.size());
  for (const auto& e : entries) {
    PluginRef ref;
    ref.name = e.name;
    ref.dir = e.dir;
    ref.title = e.name;
    // Browser-only plugins (no device.json) stay listed so an install is
    // visibly installed, but carry no manifest to open (empty manifestPath);
    // the picker opens their README instead.
    if (e.hasDevice) ref.manifestPath = e.dir + "/device.json";
    const std::string readmePath = e.dir + "/README.md";
    if (Storage.exists(readmePath.c_str())) ref.readmePath = readmePath;
    // manifest.json first, then device.json overrides (on-device authority).
    if (e.hasManifest) readTitleDesc(e.dir + "/manifest.json", ref);
    if (e.hasDevice) readTitleDesc(ref.manifestPath, ref);
    plugins.push_back(std::move(ref));
  }
  return plugins;
}

bool anyPluginInstalled() {
  // manifest.json alone means web-card metadata only; plugin.js or device.json
  // is what makes an installed plugin worth the home screen's library slot.
  for (const auto& e : PluginLocations::scanPlugins()) {
    if (e.hasPluginJs || e.hasDevice) return true;
  }
  return false;
}

bool PluginCatalogActivity::loadManifest() {
  std::string raw;
  if (!Storage.readFileToString("PCAT", manifestPath, MAX_MANIFEST_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;

  // Relative token / config paths live in the plugin's own folder.
  const std::string pluginDir = manifestPath.substr(0, manifestPath.rfind('/'));
  manifest.tokenFile = pluginhttp::inPluginDir(pluginDir, doc["token"]["file"] | "");
  manifest.tokenPath = doc["token"]["path"] | "token";
  manifest.configFile = pluginhttp::inPluginDir(pluginDir, doc["config"]["file"] | "");

  JsonVariantConst browse = doc["browse"];
  manifest.browseFormat = browse["format"] | "json";
  manifest.browseUrl = browse["url"] | "";
  manifest.browseMethod = browse["method"] | "GET";
  manifest.browseBody = browse["body"] | "";
  readHeaders(browse["headers"], manifest.browseHeaders);
  manifest.itemsPath = browse["items"] | "";
  manifest.titlePath = browse["fields"]["title"] | (manifest.isXmlList() ? "" : "title");
  manifest.authorPath = browse["fields"]["author"] | "";
  manifest.idPath = browse["fields"]["id"] | "";
  manifest.urlPath = browse["fields"]["url"] | "";
  manifest.versionPath = browse["fields"]["version"] | "";
  manifest.descriptionPath = browse["fields"]["description"] | "";
  manifest.detailAuthorPath = browse["fields"]["detail_author"] | "";
  manifest.repositoryPath = browse["fields"]["repository"] | "";
  manifest.detailsUrlPath = browse["fields"]["details_url"] | "";
  manifest.detailsFormatPath = browse["fields"]["details_format"] | "";
  if (manifest.detailAuthorPath.empty()) manifest.detailAuthorPath = manifest.authorPath;
  // Version-tracked catalogs predate item details: conventional field names
  // keep those manifests working, while newer catalogs map them explicitly.
  if (manifest.tracksInstalls()) {
    if (manifest.descriptionPath.empty()) manifest.descriptionPath = "description";
    if (manifest.repositoryPath.empty()) manifest.repositoryPath = "repository";
    if (manifest.detailsUrlPath.empty()) manifest.detailsUrlPath = "details.url";
    if (manifest.detailsFormatPath.empty()) manifest.detailsFormatPath = "details.format";
    if (manifest.detailAuthorPath.empty() || manifest.detailAuthorPath == manifest.descriptionPath) {
      manifest.detailAuthorPath = "author";
    }
  }
  const JsonVariantConst compatibility = browse["compatibility"];
  manifest.compatFamilyPath = compatibility["firmware_family"] | "";
  manifest.compatApiMinPath = compatibility["plugin_api_min"] | "";
  manifest.compatRequiresPath = compatibility["requires"] | "";
  manifest.pageSize = browse["page_size"] | 8;
  // Documented bounds: each row costs an Item (strings) and a screen slot.
  manifest.pageSize = std::min(std::max(manifest.pageSize, 1), 16);
  if (manifest.pageSize < 1) manifest.pageSize = 1;
  if (manifest.pageSize > MAX_PAGE_SIZE) manifest.pageSize = MAX_PAGE_SIZE;
  manifest.browseListsUrl = browse["lists_url"] | "";
  readHeaders(browse["lists_headers"], manifest.listsHeaders);
  manifest.listsItemsPath = browse["lists_items"] | "lists";
  const JsonVariantConst listFields = browse["lists_fields"];
  manifest.listsTitlePath = listFields["title"] | "title";
  manifest.listsUrlPath = listFields["url"] | "url";
  manifest.listsIdPath = listFields["id"] | "id";
  manifest.listsRequiresPath = listFields["requires"] | "requires";
  manifest.browseLists.reserve(std::min<size_t>(browse["lists"].size(), MAX_BROWSE_LISTS));
  for (JsonVariantConst l : browse["lists"].as<JsonArrayConst>()) {
    if (manifest.browseLists.size() >= MAX_BROWSE_LISTS) break;
    if (!matchesListRequirements(l["requires"])) continue;
    Manifest::BrowseList entry;
    entry.title = l["title"] | "";
    entry.url = l["url"] | "";
    entry.id = variantToString(l["id"]);
    readListExtras(l, entry.body, entry.noticeTitle, entry.noticeMessage, entry.noticeConfirm, entry.noticeCancel);
    if (!entry.title.empty()) manifest.browseLists.push_back(std::move(entry));
  }
  manifest.fixedListCount = manifest.browseLists.size();
  manifest.searchUrl = browse["search"]["url"] | "";
  manifest.searchBody = browse["search"]["body"] | "";
  manifest.xmlItem = browse["item"] | "";
  manifest.xmlContainer = browse["container_element"] | "";
  manifest.xmlSkipSelf = browse["skip_self"] | false;
  manifest.xmlResolveUrls = browse["resolve_urls"] | false;
  for (JsonVariantConst ext : browse["extensions"].as<JsonArrayConst>()) {
    if (ext.is<const char*>()) manifest.xmlExtensions.emplace_back(ext.as<const char*>());
  }

  JsonVariantConst dl = doc["download"];
  manifest.dlUrl = dl["url"] | "";
  manifest.dlMethod = dl["method"] | "GET";
  manifest.dlBody = dl["body"] | "";
  readHeaders(dl["headers"], manifest.dlHeaders);
  manifest.dlUrlPath = dl["url_path"] | "";
  manifest.dlUser = dl["username"] | "";
  manifest.dlPass = dl["password"] | "";
  manifest.destDir = dl["dest_dir"] | "";
  manifest.filenameTpl = dl["filename"] | "{title}.epub";
  // Multi-file bundle install (generic): base URL + a files array per item.
  manifest.bundleBasePath = dl["bundle"]["base"] | "";
  manifest.bundleFilesPath = dl["bundle"]["files"] | "";
  manifest.bundleSubdir = dl["bundle"]["subdir"] | "{id}";
  // XML-list items already carry the file URL; default the template to it.
  if (manifest.isXmlList() && manifest.dlUrl.empty()) manifest.dlUrl = "{url}";
  manifest.sidecarPath = dl["sidecar"]["path"] | "";
  manifest.sidecarBody = dl["sidecar"]["body"] | "";

  JsonVariantConst auth = doc["auth"];
  manifest.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", manifest.authReq);
  pluginhttp::readRequest(auth["poll"], "POST", manifest.pollReq);
  manifest.authCodePath = auth["code_path"] | "user_code";
  manifest.authVerifyPath = auth["verify_url_path"] | "verification_uri";
  manifest.authDeviceCodePath = auth["device_code_path"] | "device_code";
  manifest.authIntervalPath = auth["interval_path"] | "interval";
  manifest.authExpiresPath = auth["expires_path"] | "expires_in";
  manifest.authTokenPath = auth["token_path"] | "access_token";
  manifest.authErrorPath = auth["error_path"] | "error";

  return !manifest.browseUrl.empty();
}

bool PluginCatalogActivity::loadBrowseListIndex(int& status) {
  status = 200;
  if (manifest.browseListsUrl.empty()) return true;

  std::string response;
  std::string url;
  // GET with lists_headers only: a token reaches the list server only when the
  // manifest says so.
  const auto run = [&] {
    url = substituted(manifest.browseListsUrl, nullptr);
    return pluginhttp::request(session.get(), url, "GET", "", substitutedHeaders(manifest.listsHeaders, nullptr),
                               response, MAX_BROWSE_LIST_INDEX_RESPONSE);
  };
  status = run();
  // Same policy as browsing: an expired password-grant token is minted again once.
  if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) status = run();
  if (status < 200 || status >= 300) {
    LOG_ERR("PCAT", "browse list index fetch failed: status=%d %s", status, url.c_str());
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) {
    LOG_ERR("PCAT", "browse list index parse failed: %s", url.c_str());
    return false;
  }
  std::string().swap(response);  // the document holds its own copies
  JsonArrayConst lists = resolvePath(doc.as<JsonVariantConst>(), manifest.listsItemsPath).as<JsonArrayConst>();
  if (lists.isNull() || lists.size() == 0) {
    LOG_ERR("PCAT", "browse list index has no lists: %s", url.c_str());
    return false;
  }

  manifest.browseLists.resize(manifest.fixedListCount);
  manifest.browseLists.reserve(manifest.fixedListCount + std::min(lists.size(), MAX_BROWSE_LISTS));
  for (JsonVariantConst l : lists) {
    if (manifest.browseLists.size() - manifest.fixedListCount >= MAX_BROWSE_LISTS) {
      LOG_ERR("PCAT", "browse list index truncated to %u lists", static_cast<unsigned>(MAX_BROWSE_LISTS));
      break;
    }
    if (!matchesListRequirements(resolvePath(l, manifest.listsRequiresPath))) continue;
    Manifest::BrowseList entry;
    entry.title = variantToString(resolvePath(l, manifest.listsTitlePath));
    entry.url = variantToString(resolvePath(l, manifest.listsUrlPath));
    UrlUtils::resolveServerPath(entry.url, url);  // catalog.server_paths: "/x" joins the index's origin
    entry.id = variantToString(resolvePath(l, manifest.listsIdPath));
    readListExtras(l, entry.body, entry.noticeTitle, entry.noticeMessage, entry.noticeConfirm, entry.noticeCancel);
    // An entry with only an id browses the shared browse url via {list_id}.
    const bool browsable = !entry.url.empty() || !entry.id.empty();
    if (!entry.title.empty() && browsable) manifest.browseLists.push_back(std::move(entry));
  }
  if (manifest.browseLists.size() == manifest.fixedListCount) {
    LOG_ERR("PCAT", "browse list index has no valid lists: %s", url.c_str());
    return false;
  }
  return true;
}

bool PluginCatalogActivity::saveToken(const std::string& value) {
  return pluginhttp::saveTokenToFile(manifest.tokenFile, manifest.tokenPath, value);
}

bool PluginCatalogActivity::loadToken() {
  return pluginhttp::loadTokenFromFile(manifest.tokenFile, manifest.tokenPath, token);
}

void PluginCatalogActivity::loadConfig() { pluginhttp::loadConfigFile(manifest.configFile, config); }

bool PluginCatalogActivity::configReady() {
  // Only a config file that is missing (or unparseable) means "not set up";
  // keys the file leaves out substitute as empty (see substituted()), which
  // is what a server with defaults expects.
  if (manifest.configFile.empty() || !config.empty()) return true;
  LOG_ERR("PCAT", "plugin config missing or unreadable: %s", manifest.configFile.c_str());
  fail(StrId::STR_PLUGIN_NOT_CONFIGURED);
  return false;
}

void PluginCatalogActivity::fail(const StrId msg) {
  state = State::ERROR;
  errorMessage = I18N.get(msg);
  requestUpdate();
}

void PluginCatalogActivity::beginLoading() {
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
}

std::vector<std::pair<std::string, std::string>> PluginCatalogActivity::substitutedHeaders(
    const std::vector<std::pair<std::string, std::string>>& headers, const Item* item) const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(headers.size());
  for (const auto& h : headers) out.emplace_back(h.first, substituted(h.second, item));
  return out;
}

std::string PluginCatalogActivity::substituted(std::string tpl, const Item* item) const {
  substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) substituteAll(tpl, ("{cfg." + kv.first + "}").c_str(), kv.second);
  // A key the config file leaves out becomes an empty value rather than a
  // literal "{cfg.key}" on the wire: plugin READMEs document omitted keys as
  // "use the server's default", and a server cannot parse the braces anyway.
  for (size_t at = tpl.find("{cfg."); at != std::string::npos; at = tpl.find("{cfg.", at)) {
    const size_t end = tpl.find('}', at);
    if (end == std::string::npos) break;
    LOG_DBG("PCAT", "config key %s not set; substituting empty", tpl.substr(at, end - at + 1).c_str());
    tpl.erase(at, end - at + 1);
  }
  // Firmware identity, so a catalog server can tailor its answer (upstream
  // CrossPoint 2.0 template vocabulary).
  substituteAll(tpl, "{firmware_family}", firmwareidentity::FAMILY);
  substituteAll(tpl, "{firmware_version}", CROSSPOINT_VERSION);
  substituteAll(tpl, "{plugin_capabilities}", firmwareidentity::PLUGIN_CAPABILITIES);
  char num[16];
  snprintf(num, sizeof(num), "%d", firmwareidentity::PLUGIN_API_VERSION);
  substituteAll(tpl, "{plugin_api}", num);
  snprintf(num, sizeof(num), "%d", page);
  substituteAll(tpl, "{page}", num);
  // {limit} is the page size for {page}+{limit} requests (server-sized pages)
  // and page size + 1 otherwise: the extra row says another page exists.
  snprintf(num, sizeof(num), "%d", plugincatalogpaging::limitFor(browsePaging(), manifest.pageSize));
  substituteAll(tpl, "{limit}", num);
  snprintf(num, sizeof(num), "%d", std::max(0, page - 1) * manifest.pageSize);
  substituteAll(tpl, "{offset}", num);
  // The open list's id, so lists can share one browse url ("?folder={list_id_enc}").
  const bool inList = currentList >= 0 && currentList < static_cast<int>(manifest.browseLists.size());
  static const std::string noList;
  const std::string& listId = inList ? manifest.browseLists[currentList].id : noList;
  substituteAll(tpl, "{list_id}", listId);
  if (tpl.find("{list_id_enc}") != std::string::npos) substituteAll(tpl, "{list_id_enc}", urlEncodeQuery(listId));
  substituteAll(tpl, "{query}", urlEncodeQuery(searchQuery));
  substituteAll(tpl, "{query_raw}", searchQuery);
  if (item) {
    substituteAll(tpl, "{id}", item->id);
    substituteAll(tpl, "{title}", item->title);
    substituteAll(tpl, "{author}", item->author);
    substituteAll(tpl, "{url}", item->url);
    // Percent-encoded forms for query values and path segments, built only
    // when the template uses them: a field can be KBs and its encoding three
    // times that.
    const std::pair<const char*, const std::string*> encoded[] = {{"{id_enc}", &item->id},
                                                                  {"{title_enc}", &item->title},
                                                                  {"{author_enc}", &item->author},
                                                                  {"{url_enc}", &item->url}};
    for (const auto& [key, value] : encoded) {
      if (tpl.find(key) != std::string::npos) substituteAll(tpl, key, urlEncodeQuery(*value));
    }
  }
  return tpl;
}

PluginCatalogActivity::PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("PluginCatalog", renderer, mappedInput) {}

PluginCatalogActivity::~PluginCatalogActivity() = default;

int PluginCatalogActivity::apiRequest(const std::string& url, const std::string& method, const std::string& body,
                                      const std::vector<std::pair<std::string, std::string>>& headers,
                                      std::string& out) {
  return pluginhttp::request(session.get(), url, method, body, headers, out, MAX_API_RESPONSE);
}

int PluginCatalogActivity::apiRequestToFile(const std::string& url, const std::string& method, const std::string& body,
                                            const std::vector<std::pair<std::string, std::string>>& headers,
                                            const char* destPath) {
  return pluginhttp::requestToFile(session.get(), url, method, body, headers, destPath, MAX_BROWSE_RESPONSE);
}

void PluginCatalogActivity::onEnter() {
  Activity::onEnter();
  enterPluginPicker();
}

void PluginCatalogActivity::enterPluginPicker() {
  installedPlugins = discoverPlugins();

  manifestPath.clear();
  manifest = Manifest{};
  catalogTitle = tr(STR_PLUGINS);
  token.clear();
  config.clear();
  items.clear();
  page = 1;
  hasMore = false;
  currentList = -1;
  searchActive = false;
  searchQuery.clear();
  browseHistory.clear();
  browseCurrentUrl.clear();
  errorMessage.clear();
  session.reset();  // no TLS while only picking
  selectedIndex = 0;
  pickerConfirmDown = false;
  state = State::PLUGIN_PICKER;
  if (pickerReturnRow > 0 && pickerReturnRow < rowCount()) selectedIndex = pickerReturnRow;
  requestUpdate();
}

void PluginCatalogActivity::enterCatalog() {
  state = State::CHECK_WIFI;
  items.clear();
  selectedIndex = 0;
  page = 1;
  hasMore = false;
  currentList = -1;
  listIndexLoaded = false;
  searchActive = false;
  searchQuery.clear();
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  session.reset(new (std::nothrow) freeink::SecureHttpClient());
  if (session) session->setReuse(true);

  if (!loadManifest()) {
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
    return;
  }
  requestUpdate();
  checkAndConnectWifi();
}

// Leaving the open catalog (Back at its root, or Back out of an error /
// sign-in screen): return to the plugin picker. Wi-Fi stays up so the next
// pick connects instantly; onExit tears it down when the activity ends.
void PluginCatalogActivity::exitCatalog() {
  Storage.remove(BROWSE_TMP_PATH);
  enterPluginPicker();
}

void PluginCatalogActivity::onExit() {
  Activity::onExit();
  items.clear();
  session.reset();  // drop the reused TLS session before Wi-Fi teardown
  Storage.remove(BROWSE_TMP_PATH);
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void PluginCatalogActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    startBrowse();
    return;
  }
  launchWifiSelection();
}

bool PluginCatalogActivity::wantsListPicker() const {
  // Browse lists apply to JSON catalogs; XML lists navigate by folder instead.
  return !manifest.browseLists.empty() && !manifest.isXmlList() && currentList < 0;
}

void PluginCatalogActivity::startBrowse() {
  const bool needsListIndex = !manifest.browseListsUrl.empty() && !manifest.isXmlList() && !listIndexLoaded;
  if (needsListIndex || wantsListPicker()) {
    // Same auth gate as fetchPage: without it a signed-out user is shown the
    // list picker and only hits the sign-in screen after picking a list. The
    // list index may use {token}/{cfg.KEY} too. loadToken() returns true for
    // token-less catalogs, which skip the gate.
    loadConfig();
    if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
      if (manifest.hasDeviceCode()) {
        beginAuth();  // straight to the QR/code sign-in, no interstitial
      } else {
        state = State::NO_TOKEN;
        requestUpdate();
      }
      return;
    }
  }
  if (needsListIndex) {
    beginLoading();
    int status = 0;
    if (!loadBrowseListIndex(status)) {
      if (status == 401 || status == 403) {
        state = State::NO_TOKEN;  // as for a rejected browse request: sign in again
        requestUpdate();
        return;
      }
      // Without manifest lists to fall back on the catalog has nothing to show.
      if (manifest.fixedListCount == 0) {
        fail(StrId::STR_FETCH_FEED_FAILED);
        return;
      }
    }
    // After a failed fetch the manifest's own lists still work; Back to the
    // picker must not refetch until the catalog is reopened.
    listIndexLoaded = true;
  }
  if (wantsListPicker()) {
    if (manifest.browseLists.size() == 1) {
      openList(0);  // a one-entry picker is just an extra click
      return;
    }
    items.clear();
    page = 1;
    hasMore = false;
    selectedIndex = 0;
    state = State::LIST_PICKER;
    requestUpdate();
    return;
  }
  beginLoading();
  fetchPage(1);
}

void PluginCatalogActivity::openList(const int index) {
  if (index < 0 || index >= static_cast<int>(manifest.browseLists.size())) return;
  const auto& list = manifest.browseLists[index];
  if (!list.hasNotice()) {
    currentList = index;
    startBrowse();
    return;
  }
  // The notice (catalog.notices): Confirm continues into the list; Back stays
  // on the picker, or leaves the catalog when this is its only list. Labels
  // default to the translated Confirm / Cancel.
  const std::string confirm = substituted(list.noticeConfirm, nullptr);
  const std::string cancel = substituted(list.noticeCancel, nullptr);
  infoOpen = true;
  state = State::LIST_PICKER;  // the screen under the notice
  startActivityForResult(
      std::make_unique<PluginInfoActivity>(
          renderer, mappedInput, substituted(list.noticeTitle.empty() ? list.title : list.noticeTitle, nullptr),
          substituted(list.noticeMessage, nullptr), confirm.empty() ? std::string(tr(STR_CONFIRM)) : confirm,
          cancel.empty() ? std::string(tr(STR_CANCEL)) : cancel),
      [this, index](const ActivityResult& result) {
        infoOpen = false;
        if (result.isCancelled) {
          if (manifest.browseLists.size() == 1) {
            exitCatalog();
          } else {
            requestUpdate();
          }
          return;
        }
        currentList = index;
        startBrowse();
      });
}

void PluginCatalogActivity::retryBrowse() {
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    launchWifiSelection();
  } else if (state == State::NO_TOKEN && manifest.hasDeviceCode()) {
    beginAuth();
  } else if ((!manifest.browseListsUrl.empty() && !manifest.isXmlList() && !listIndexLoaded) || wantsListPicker()) {
    startBrowse();  // nothing picked yet: retry lands on the list picker
  } else {
    beginLoading();
    fetchPage(page);
  }
}

void PluginCatalogActivity::pumpDownloadInput() {
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
  // The transfer runs synchronously, so the main loop's action-bar dispatch
  // never sees a tap: any tap on the Downloading screen is the Cancel the
  // footer offers.
  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) cancelDownload = true;
  if (mappedInput.wasHomeGesture()) {
    cancelDownload = true;
    goHomeAfterCancel = true;
  }
}

// Shared progress callback for single-file and bundle downloads: updates the
// byte counter, pumps input for cancel, and throttles repaints to visible
// percent steps.
void PluginCatalogActivity::onDownloadProgress(const size_t downloaded, const size_t total) {
  downloadProgress = downloaded;
  downloadTotal = total;
  pumpDownloadInput();
  const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
  const unsigned long now = millis();
  if (percent >= 100 || dlLastRenderedPercent < 0 ||
      percent >= dlLastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
      now - dlLastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
    dlLastRenderedPercent = percent;
    dlLastProgressUpdateMs = now;
    requestUpdate(true);
  }
}

void PluginCatalogActivity::finishCancelledDownload() {
  LOG_INF("PCAT", "Download cancelled");
  if (goHomeAfterCancel) {
    onGoHome();
    return;
  }
  state = State::BROWSING;
  requestUpdate();
}

void PluginCatalogActivity::launchSearch() {
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void PluginCatalogActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    requestUpdate();
    return;
  }
  searchQuery = query;
  searchActive = true;
  currentList = -1;  // search spans the whole catalog, not a single list
  selectedIndex = 0;
  beginLoading();
  fetchPage(1);
}

void PluginCatalogActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             startBrowse();
                           } else {
                             fail(StrId::STR_WIFI_CONN_FAILED);
                           }
                         });
}

bool PluginCatalogActivity::refreshCredentialToken() {
  loadConfig();
  std::string minted;
  if (!pluginhttp::mintPasswordToken(session.get(), substituted(manifest.authReq.url, nullptr), manifest.authReq.method,
                                     substituted(manifest.authReq.body, nullptr),
                                     substitutedHeaders(manifest.authReq.headers, nullptr), manifest.authTokenPath,
                                     minted)) {
    return false;
  }
  if (!saveToken(minted)) return false;
  token = minted;  // usable immediately, without re-reading the file
  return true;
}

int PluginCatalogActivity::browseRequestToFile(const std::string& urlTemplate, const std::string& bodyTemplate,
                                               const char* destPath) {
  auto build = [&](std::string& url, std::string& body, std::vector<std::pair<std::string, std::string>>& headers) {
    url = substituted(urlTemplate, nullptr);
    body = substituted(bodyTemplate, nullptr);
    headers = substitutedHeaders(manifest.browseHeaders, nullptr);
  };
  std::string url, body;
  std::vector<std::pair<std::string, std::string>> headers;
  build(url, body, headers);
  int status = apiRequestToFile(url, manifest.browseMethod, body, headers, destPath);
  // A password-grant token expires; on 401/403 mint a fresh one and retry once.
  if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) {
    build(url, body, headers);
    status = apiRequestToFile(url, manifest.browseMethod, body, headers, destPath);
  }
  return status;
}

void PluginCatalogActivity::fetchXmlList() {
  loadConfig();
  if (!configReady()) return;
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (manifest.xmlItem.empty()) {
    fail(StrId::STR_PLUGIN_MANIFEST_INVALID);
    return;
  }
  if (browseCurrentUrl.empty()) browseCurrentUrl = substituted(manifest.browseUrl, nullptr);

  const int status = browseRequestToFile(browseCurrentUrl, manifest.browseBody, BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    Storage.remove(BROWSE_TMP_PATH);
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {  // 207 Multi-Status counts as success
    Storage.remove(BROWSE_TMP_PATH);
    fail(StrId::STR_FETCH_FEED_FAILED);
    return;
  }

  const std::string origin = originOf(browseCurrentUrl);
  const std::string selfPath = pathOf(browseCurrentUrl);
  auto trimSlash = [](std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
  };
  const std::string decodedSelf = trimSlash(urlDecode(selfPath));

  // Extract one row per repeating item element, then apply the list rules.
  const std::string selectors[XmlListParser::F_COUNT] = {manifest.urlPath, manifest.titlePath, manifest.authorPath,
                                                         manifest.idPath};
  XmlListParser parser(manifest.xmlItem, manifest.xmlContainer, selectors);
  std::vector<XmlListParser::RawItem> rows;
  {
    HalFile file;
    if (Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) rows = parser.parseFile(file);
    // file closed at scope exit, before the remove below
  }
  Storage.remove(BROWSE_TMP_PATH);

  items.clear();
  items.reserve(rows.size());
  for (const auto& row : rows) {
    const std::string& rawUrl = row.field[XmlListParser::F_URL];
    if (rawUrl.empty()) continue;
    Item item;
    item.isDir = row.isDir;
    item.url =
        manifest.xmlResolveUrls && rawUrl.rfind("http", 0) != 0 ? origin + urlEncodePath(urlDecode(rawUrl)) : rawUrl;
    item.author = row.field[XmlListParser::F_AUTHOR];
    item.id = row.field[XmlListParser::F_ID];
    const std::string& title = row.field[XmlListParser::F_TITLE];
    item.title = title.empty() ? basename(urlDecode(rawUrl)) : title;

    if (manifest.xmlSkipSelf && trimSlash(urlDecode(rawUrl)) == decodedSelf) continue;
    if (!item.isDir && !hasAllowedExtension(urlDecode(rawUrl), manifest.xmlExtensions)) continue;
    items.push_back(std::move(item));
  }

  // Folders first, then files, each alphabetical — matches how file managers list.
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.isDir != b.isDir) return a.isDir;
    return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
  });
  hasMore = false;
  selectedIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

bool PluginCatalogActivity::prevRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && page > 1;
}

bool PluginCatalogActivity::nextRowVisible() const {
  return state == State::BROWSING && !manifest.isXmlList() && hasMore;
}

int PluginCatalogActivity::rowCount() const {
  if (state == State::PLUGIN_PICKER) return static_cast<int>(installedPlugins.size());
  if (state == State::LIST_PICKER) return static_cast<int>(manifest.browseLists.size());
  if (state != State::BROWSING) return 0;
  return static_cast<int>(items.size()) + (prevRowVisible() ? 1 : 0) + (nextRowVisible() ? 1 : 0);
}

const std::string& PluginCatalogActivity::activeBrowseUrl() const {
  // Search overrides the list view, reusing the browse url when unspecified.
  if (searchActive) return pick(manifest.searchUrl, manifest.browseUrl);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseUrl;
  return pick(manifest.browseLists[currentList].url, manifest.browseUrl);
}

const std::string& PluginCatalogActivity::activeBrowseBody() const {
  if (searchActive) return pick(manifest.searchBody, manifest.browseBody);
  if (currentList < 0 || currentList >= static_cast<int>(manifest.browseLists.size())) return manifest.browseBody;
  return pick(manifest.browseLists[currentList].body, manifest.browseBody);
}

plugincatalogpaging::Paging PluginCatalogActivity::browsePaging() const {
  return plugincatalogpaging::pagingOf(activeBrowseUrl(), activeBrowseBody(), manifest.browseHeaders);
}

void PluginCatalogActivity::fetchPage(const int newPage) {
  if (manifest.isXmlList()) {
    fetchXmlList();
    return;
  }
  page = newPage;
  loadConfig();
  if (!configReady()) return;
  if (!loadToken() && !(manifest.hasPasswordGrant() && refreshCredentialToken())) {
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }

  const int status = browseRequestToFile(activeBrowseUrl(), activeBrowseBody(), BROWSE_TMP_PATH);
  if (status == 401 || status == 403) {
    // Stale or revoked token: back to the sign-in screen, not a raw error.
    Storage.remove(BROWSE_TMP_PATH);
    state = State::NO_TOKEN;
    requestUpdate();
    return;
  }
  if (status < 200 || status >= 300) {
    Storage.remove(BROWSE_TMP_PATH);
    fail(StrId::STR_FETCH_FEED_FAILED);
    return;
  }

  JsonDocument filter;
  addFieldFilter(filter, manifest.itemsPath, manifest.titlePath);
  addFieldFilter(filter, manifest.itemsPath, manifest.authorPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.idPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.urlPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.versionPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.bundleBasePath);
  addFieldFilter(filter, manifest.itemsPath, manifest.bundleFilesPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.compatFamilyPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.compatApiMinPath);
  addFieldFilter(filter, manifest.itemsPath, manifest.compatRequiresPath);

  // Filtered parse straight from the SD temp file — the raw response never
  // occupies DRAM, only the few fields the filter admits.
  JsonDocument doc;
  {
    HalFile file;
    if (!Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) {
      fail(StrId::STR_PARSE_FEED_FAILED);
      return;
    }
    struct HalFileReader {
      HalFile& f;
      int read() { return f.read(); }
      size_t readBytes(char* buf, size_t n) {
        const int r = f.read(buf, n);
        return r < 0 ? 0 : static_cast<size_t>(r);
      }
    } reader{file};
    const auto parseErr = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    // file closed at scope exit, before the remove below
    if (parseErr != DeserializationError::Ok) {
      LOG_ERR("PCAT", "browse JSON parse error: %s", parseErr.c_str());
      Storage.remove(BROWSE_TMP_PATH);
      fail(StrId::STR_PARSE_FEED_FAILED);
      return;
    }
  }
  // The response stays on SD while this page is shown: a Details screen reads
  // its row's long fields from it on demand. Replaced by the next fetch and
  // removed when the catalog closes.
  if (!manifest.hasItemDetails()) Storage.remove(BROWSE_TMP_PATH);

  JsonVariantConst itemsNode = resolvePath(doc.as<JsonVariantConst>(), manifest.itemsPath);
  JsonArrayConst arr = itemsNode.as<JsonArrayConst>();
  items.clear();
  // The variables the request uses decide the paging (upstream CrossPoint
  // #4170/#4195/#4221): {page}/{offset} requests are already sliced by the
  // server, and every row it sent counts toward its page, shown or not; a
  // static feed (neither variable) holds every row, so the page window slides
  // over the rows we show. Without that a static feed repeated page 1 forever.
  const plugincatalogpaging::Paging paging = browsePaging();
  const size_t pageOffset = paging == plugincatalogpaging::Paging::Static
                                ? static_cast<size_t>(std::max(0, page - 1)) * static_cast<size_t>(manifest.pageSize)
                                : 0;
  plugincatalogpaging::PageWindow window(pageOffset, static_cast<size_t>(manifest.pageSize));
  if (!arr.isNull()) {
    items.reserve(manifest.pageSize);
    int rowIndex = -1;
    for (JsonVariantConst v : arr) {
      rowIndex++;
      // An incompatible entry is never shown; on a static feed the page window
      // then slides over compatible entries only, so pages never come out empty.
      const bool shown = !variantToString(resolvePath(v, manifest.titlePath)).empty() &&
                         (!manifest.filtersCompatibility() ||
                          catalogEntryCompatible(variantToString(resolvePath(v, manifest.compatFamilyPath)),
                                                 variantToString(resolvePath(v, manifest.compatApiMinPath)),
                                                 csvField(resolvePath(v, manifest.compatRequiresPath))));
      if (!window.take(plugincatalogpaging::countsTowardPage(paging, shown)) || !shown) continue;
      Item item;
      item.sourceIndex = rowIndex;
      item.title = variantToString(resolvePath(v, manifest.titlePath));
      item.author = variantToString(resolvePath(v, manifest.authorPath));
      item.id = variantToString(resolvePath(v, manifest.idPath));
      item.url = variantToString(resolvePath(v, manifest.urlPath));
      if (manifest.tracksInstalls()) item.version = variantToString(resolvePath(v, manifest.versionPath));
      if (manifest.isBundle()) {
        item.base = variantToString(resolvePath(v, manifest.bundleBasePath));
        JsonArrayConst fileArr = resolvePath(v, manifest.bundleFilesPath).as<JsonArrayConst>();
        if (!fileArr.isNull()) {
          item.files.reserve(fileArr.size());
          for (JsonVariantConst f : fileArr) {
            if (f.is<const char*>()) item.files.emplace_back(f.as<const char*>());
          }
        }
      }
      items.push_back(std::move(item));
    }
  }
  hasMore = window.hasMore(paging);
  computeInstallStatus();
  selectedIndex = 0;
  state = State::BROWSING;
  requestUpdate();
}

// Badge each item by comparing its catalog version to the installed copy's
// manifest (located by folder id across the plugin roots). Only a newer
// catalog version (MAJOR.MINOR.PATCH) is an update, so an older catalog never
// offers a downgrade (upstream CrossPoint 2.0). Runs once per page.
void PluginCatalogActivity::computeInstallStatus() {
  if (!manifest.tracksInstalls()) return;
  JsonDocument filter;
  filter["version"] = true;
  for (auto& item : items) {
    item.status.clear();
    if (item.id.empty()) continue;
    // Locate the install across every plugin root (not just one), matching
    // the discovery the rest of the firmware uses.
    const std::string dir = PluginLocations::findPluginDir(item.id.c_str());
    std::string raw;
    if (dir.empty() || !Storage.readFileToString("PCAT", dir + "/manifest.json", MAX_MANIFEST_SIZE, raw)) {
      // Not installed: show the available version so the row is not blank.
      if (!item.version.empty()) item.status = "v" + item.version;
      continue;
    }
    JsonDocument doc;
    std::string installed;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
      installed = doc["version"] | "";
    }
    item.status =
        PluginLocations::isNewerVersion(item.version, installed) ? tr(STR_UPDATE_AVAILABLE) : tr(STR_INSTALLED);
  }
}

void PluginCatalogActivity::beginAuth() {
  beginLoading();

  const auto headers = substitutedHeaders(manifest.authReq.headers, nullptr);
  std::string response;
  const int status = apiRequest(substituted(manifest.authReq.url, nullptr), manifest.authReq.method,
                                substituted(manifest.authReq.body, nullptr), headers, response);
  JsonDocument doc;
  if (status < 200 || status >= 300 || deserializeJson(doc, response) != DeserializationError::Ok) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  authUserCode = variantToString(resolvePath(root, manifest.authCodePath));
  authVerifyUrl = variantToString(resolvePath(root, manifest.authVerifyPath));
  authDeviceCode = variantToString(resolvePath(root, manifest.authDeviceCodePath));
  const long interval = resolvePath(root, manifest.authIntervalPath) | 5L;
  const long expires = resolvePath(root, manifest.authExpiresPath) | 900L;
  if (authUserCode.empty() || authDeviceCode.empty()) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
    return;
  }
  authIntervalMs = (interval < 3 ? 3 : interval) * 1000UL;
  authDeadlineMs = millis() + (expires < 60 ? 60 : expires) * 1000UL;
  authNextPollMs = millis() + authIntervalMs;
  state = State::AUTH;
  requestUpdate();
}

void PluginCatalogActivity::pollAuth() {
  authNextPollMs = millis() + authIntervalMs;

  const auto headers = substitutedHeaders(manifest.pollReq.headers, nullptr);
  std::string url = substituted(manifest.pollReq.url, nullptr);
  std::string body = substituted(manifest.pollReq.body, nullptr);
  substituteAll(url, "{device_code}", authDeviceCode);
  substituteAll(body, "{device_code}", authDeviceCode);

  std::string response;
  const int status = apiRequest(url, manifest.pollReq.method, body, headers, response);
  if (status < 0) return;  // transient transport failure: keep polling

  JsonDocument doc;
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    const JsonVariantConst root = doc.as<JsonVariantConst>();
    const std::string newToken = variantToString(resolvePath(root, manifest.authTokenPath));
    if (!newToken.empty()) {
      if (!saveToken(newToken)) {
        fail(StrId::STR_PLUGIN_AUTH_FAILED);
        return;
      }
      startBrowse();
      return;
    }
    const std::string code = variantToString(resolvePath(root, manifest.authErrorPath));
    if (code == "slow_down") {
      authIntervalMs += 5000;
    } else if (code == "expired_token" || code == "access_denied") {
      fail(StrId::STR_PLUGIN_AUTH_FAILED);
      return;
    }
    // authorization_pending (or anything unrecognized): keep polling
  }

  if (static_cast<long>(millis() - authDeadlineMs) >= 0) {
    fail(StrId::STR_PLUGIN_AUTH_FAILED);
  }
}

void PluginCatalogActivity::downloadItem(const Item& item) {
  state = State::DOWNLOADING;
  statusMessage = item.title;
  downloadProgress = 0;
  downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

  // Multi-file bundle install: fetch every file in item.files from item.base
  // into destDir/<subdir>/, creating intermediate folders. Each file reports
  // its transferred byte count. Generic (a plugin installer, a theme pack, ...).
  if (manifest.isBundle() && !item.files.empty()) {
    const std::string subdir = substituted(manifest.bundleSubdir, &item);
    // Reject path traversal in the subdir (a hostile catalog could escape).
    if (subdir.empty() || subdir.find("..") != std::string::npos || subdir.front() == '/') {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    std::string dir = manifest.destDir;
    if (!dir.empty() && dir.back() == '/') dir.pop_back();
    dir += '/';
    dir += subdir;
    if (!Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
      LOG_ERR("PCAT", "bundle mkdir failed: %s", dir.c_str());
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    std::string base = item.base;
    if (!base.empty() && base.back() != '/') base += '/';
    const size_t total = item.files.size();
    // Written files, for rollback: a half-installed bundle folder would show
    // up as a broken plugin/theme in the next discovery scan.
    std::vector<std::string> written;
    written.reserve(total);
    const auto rollback = [&] {
      for (const auto& path : written) Storage.remove(path.c_str());
      Storage.rmdir(dir.c_str());  // only succeeds when the folder emptied out
    };
    for (size_t i = 0; i < total; i++) {
      std::string rel = item.files[i];
      while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
      if (rel.empty() || rel.find("..") != std::string::npos) {
        // A manifest listing traversal entries is hostile or broken either
        // way; abort rather than install a bundle with silent holes.
        LOG_ERR("PCAT", "unsafe bundle entry rejected: %s", item.files[i].c_str());
        rollback();
        fail(StrId::STR_DOWNLOAD_FAILED);
        return;
      }
      downloadProgress = 0;
      const std::string dest = dir + "/" + rel;
      // Create any intermediate folders for nested files ("assets/icon.bin").
      const size_t slash = dest.find_last_of('/');
      if (slash != std::string::npos) {
        const std::string parent = dest.substr(0, slash);
        if (!Storage.exists(parent.c_str())) Storage.mkdir(parent.c_str());
      }
      dlLastRenderedPercent = -1;
      dlLastProgressUpdateMs = 0;
      // allowConfiguredAuth=false: the configured OPDS credentials must never
      // be attached to a plugin's own server; expectedSize=0 (bundles carry no
      // advertised size).
      const auto res = HttpDownloader::downloadToFile(
          pluginhttp::escapeUrl(base + rel), dest,
          [this](const size_t downloaded, const size_t total) { onDownloadProgress(downloaded, total); }, false, 0,
          &cancelDownload);
      if (res == HttpDownloader::ABORTED) {
        rollback();
        finishCancelledDownload();
        return;
      }
      if (res != HttpDownloader::OK) {
        LOG_ERR("PCAT", "bundle file failed: %s (%d)", rel.c_str(), static_cast<int>(res));
        rollback();
        fail(StrId::STR_DOWNLOAD_FAILED);
        return;
      }
      written.push_back(dest);
    }
    state = State::DONE;
    statusMessage = item.title;
    requestUpdate();
    return;
  }

  // Resolve the file URL: either the template itself, or one API hop away.
  std::string fileUrl = substituted(manifest.dlUrl, &item);
  // The file GET carries the download headers unless url_path answered with a
  // full URL, which may be pre-signed and reject a second Authorization. A
  // server path ("/files/1", catalog.server_paths) is on the API's own host,
  // so it keeps them (upstream CrossPoint #4273).
  bool sendDownloadHeaders = manifest.dlUrlPath.empty();
  if (!manifest.dlUrlPath.empty()) {
    const std::string hopUrl = fileUrl;
    std::string response;
    int status = apiRequest(hopUrl, manifest.dlMethod, substituted(manifest.dlBody, &item),
                            substitutedHeaders(manifest.dlHeaders, &item), response);
    // An expired password-grant token: mint a fresh one and retry once.
    if ((status == 401 || status == 403) && manifest.hasPasswordGrant() && refreshCredentialToken()) {
      status = apiRequest(substituted(manifest.dlUrl, &item), manifest.dlMethod, substituted(manifest.dlBody, &item),
                          substitutedHeaders(manifest.dlHeaders, &item), response);
    }
    if (status < 200 || status >= 300) {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response) != DeserializationError::Ok) {
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    fileUrl = variantToString(resolvePath(doc.as<JsonVariantConst>(), manifest.dlUrlPath));
    sendDownloadHeaders = UrlUtils::resolveServerPath(fileUrl, hopUrl);
  }
  if (fileUrl.empty()) {
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }

  const char* folder = manifest.destDir.c_str();
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("PCAT", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // Sanitize after substitution so the byte limit applies to the complete
  // filename and does not remove an extension already present in {title}.
  const std::string filename =
      StringUtils::sanitizeFilenamePreservingExtension(substituted(manifest.filenameTpl, &item));
  if (filename.empty() || filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
    LOG_ERR("PCAT", "unsafe filename rejected: %s", filename.c_str());
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }
  std::string dest;
  dest.reserve((haveFolder ? manifest.destDir.size() : 0) + 1 + filename.size());
  if (haveFolder) dest += folder;
  dest += '/';
  dest += filename;

  const std::string dlUser = substituted(manifest.dlUser, &item);
  const std::string dlPass = substituted(manifest.dlPass, &item);
  const auto fetchFile = [&] {
    const auto dlHeaders = sendDownloadHeaders ? substitutedHeaders(manifest.dlHeaders, &item)
                                               : std::vector<std::pair<std::string, std::string>>{};
    dlLastRenderedPercent = -1;
    dlLastProgressUpdateMs = 0;
    return HttpDownloader::downloadToFile(
        pluginhttp::escapeUrl(fileUrl), dest,
        [this](const size_t downloaded, const size_t total) { onDownloadProgress(downloaded, total); }, false, 0,
        &cancelDownload, dlUser, dlPass, dlHeaders);
  };
  auto result = fetchFile();
  // A GET carrying the catalog's token templates gets one mint-and-retry when
  // an expired password-grant token is refused.
  const bool refused = result == HttpDownloader::HTTP_STATUS_ERROR &&
                       (HttpDownloader::lastHttpStatus() == 401 || HttpDownloader::lastHttpStatus() == 403);
  if (refused && sendDownloadHeaders && manifest.hasPasswordGrant() && refreshCredentialToken()) {
    result = fetchFile();
  }

  if (result == HttpDownloader::ABORTED) {
    finishCancelledDownload();
    return;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR("PCAT", "Download failed: %d", static_cast<int>(result));
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }
  clearBookCache(dest);

  // Optional per-book sidecar (e.g. a service book id keyed by the file's
  // path hash) so a later sync stage can associate the file with the service.
  if (!manifest.sidecarPath.empty() && !manifest.sidecarBody.empty()) {
    Item sidecarItem = item;
    const std::string md5 = md5Hex(dest);
    std::string path = substituted(manifest.sidecarPath, &sidecarItem);
    substituteAll(path, "{md5}", md5);
    // {dest} is the sanitized on-SD path of the downloaded file, so a sidecar
    // can sit next to it ("{dest}.meta.json" - the book metadata convention);
    // {title} alone cannot express that, since the filename is sanitized.
    substituteAll(path, "{dest}", dest);
    // Sidecar paths legitimately contain '/', but the substituted fields must
    // not climb out of the tree.
    if (path.empty() || path.find("..") != std::string::npos) {
      LOG_ERR("PCAT", "unsafe sidecar path rejected: %s", path.c_str());
    } else {
      std::string body = substituted(manifest.sidecarBody, &sidecarItem);
      substituteAll(body, "{md5}", md5);
      substituteAll(body, "{dest}", dest);
      HalFile sidecar;
      if (Storage.openFileForWrite("PCAT", path, sidecar)) {
        if (sidecar.write(body.data(), body.size()) != body.size()) {
          LOG_ERR("PCAT", "short sidecar write: %s", path.c_str());
          if (sidecar.isOpen()) sidecar.close();
          Storage.remove(path.c_str());
        } else {
          sidecar.flush();
        }
      } else {
        LOG_ERR("PCAT", "Sidecar write failed: %s", path.c_str());
      }
    }
  }

  state = State::DONE;
  statusMessage = item.title;
  requestUpdate();
}

// --- Input ------------------------------------------------------------------

void PluginCatalogActivity::loop() {
  // Child activities (Wi-Fi picker, keyboard, plugin info) own the input while
  // they are up; the download runs synchronously and pumps its own input.
  if (state == State::WIFI_SELECTION || state == State::DOWNLOADING || infoOpen) return;

  if (state == State::AUTH) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::NO_TOKEN;
      requestUpdate();
      return;
    }
    if (static_cast<long>(millis() - authNextPollMs) >= 0) pollAuth();
    return;
  }

  if (state == State::ERROR || state == State::NO_TOKEN) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      retryBrowse();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitCatalog();
    }
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) exitCatalog();
    return;
  }

  if (state == State::DONE) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(tx, ty)) {
      // The just-finished download may have installed/updated a plugin; refresh
      // the install badges so the row no longer reads "Update".
      computeInstallStatus();
      state = State::BROWSING;
      requestUpdate();
    }
    return;
  }

  handleListInput();
}

// The three list states share one input model, the same as the BookFusion
// browser: Full Touch taps select then activate a row (TouchListNav), Confirm
// activates the highlighted row, Back climbs out, a tap on the side keys steps
// the selection and a hold (or a vertical swipe) pages it.
void PluginCatalogActivity::handleListInput() {
  const int total = rowCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return;
  }

  // On the top row of a searchable catalog the previous-nav slot becomes
  // Search (front Left / Up), mirroring the OPDS browser's side-button search.
  // The X4 Pro's action bar draws that slot too (drawFooter passes allSlots).
  if (state == State::BROWSING && manifest.hasSearch() && selectedIndex == 0) {
    const auto buttons = ButtonNavigator::getPreviousButtons();
    const bool previousNavReleased =
        std::any_of(buttons.begin(), buttons.end(),
                    [this](const MappedInputManager::Button b) { return mappedInput.wasReleased(b); });
    if (previousNavReleased) {
      launchSearch();
      return;
    }
  }

  if (state == State::PLUGIN_PICKER && handlePickerHold()) return;

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), total, selectedIndex, rowsHaveSubtitle(), tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      activateIndex(selectedIndex);
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  // Press, not release, like Settings and the BookFusion browser: the press
  // that opened this screen from the Settings row is already consumed there,
  // so its release cannot open the first plugin on its own.
  if (state != State::PLUGIN_PICKER && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (total > 0) activateIndex(selectedIndex);
    return;
  }
  if (total <= 0) return;

  // Visual page size, the same calculation drawList uses, so a page jump
  // matches one screen of rows exactly.
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, rowsHaveSubtitle());
  buttonNavigator.onNextRelease([this, total] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, total] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });
  if (TouchListNav::pageSwipe(mappedInput, total, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }
  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
}

bool PluginCatalogActivity::handlePickerHold() {
  // Touch: a long-press on a row offers the uninstall. It fires while the
  // finger is down, so the rest of the contact is dropped (its lift would
  // otherwise also arrive as a tap on the row).
  int lx = 0;
  int ly = 0;
  if (mappedInput.wasScreenLongPress(lx, ly)) {
    const int index = GUI.hitTestList(listRect(), rowCount(), selectedIndex, rowsHaveSubtitle(), lx, ly);
    if (index >= 0) {
      mappedInput.suppressTouchContact();
      selectedIndex = index;
      confirmUninstallPlugin(index);
      return true;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    pickerConfirmDown = true;
    return true;
  }
  if (!pickerConfirmDown) return false;
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime(MappedInputManager::Button::Confirm) >= PLUGIN_UNINSTALL_HOLD_MS) {
      pickerConfirmDown = false;  // the release that follows is not a tap
      confirmUninstallPlugin(selectedIndex);
    }
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    pickerConfirmDown = false;
    if (rowCount() > 0) activateIndex(selectedIndex);
    return true;
  }
  pickerConfirmDown = false;  // released between frames without a release edge
  return false;
}

void PluginCatalogActivity::confirmUninstallPlugin(const int index) {
  if (state != State::PLUGIN_PICKER || index < 0 || index >= static_cast<int>(installedPlugins.size())) return;
  const PluginRef plugin = installedPlugins[index];
  // Only ever a direct child of a plugin root, named like the folder.
  bool safeDir = !plugin.name.empty() && plugin.name != "." && plugin.name != ".." &&
                 plugin.name.find_first_of("/\\") == std::string::npos;
  if (safeDir) {
    safeDir = false;
    for (const char* root : PluginLocations::kRoots) {
      if (plugin.dir == std::string(root) + "/" + plugin.name) safeDir = true;
    }
  }
  if (!safeDir) {
    LOG_ERR("PCAT", "Refusing unsafe plugin uninstall: %s", plugin.dir.c_str());
    return;
  }
  infoOpen = true;  // parks the picker's input under the dialog
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_PLUGIN_UNINSTALL_CONFIRM), plugin.title),
      [this, plugin, index](const ActivityResult& result) {
        infoOpen = false;
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const bool removed = !Storage.exists(plugin.dir.c_str()) || Storage.removeDir(plugin.dir.c_str());
        if (removed) {
          LOG_INF("PCAT", "Uninstalled plugin %s", plugin.dir.c_str());
        } else {
          LOG_ERR("PCAT", "Failed to uninstall plugin: %s", plugin.dir.c_str());
        }
        // Keep the cursor near where it was: the next row moves up into it.
        pickerReturnRow = index > 0 && index >= static_cast<int>(installedPlugins.size()) - 1 ? index - 1 : index;
        enterPluginPicker();
      });
}

// Back on the picker leaves the activity. The catalog states route their Back
// through exitCatalog() instead, landing back on the picker.
void PluginCatalogActivity::onBackButton() {
  if (state == State::PLUGIN_PICKER) {
    finish();  // the caller (Settings, File Transfer picker) pushed us
    return;
  }
  if (searchActive) {
    // Leave the results and return to the pre-search view (picker or page 1).
    searchActive = false;
    searchQuery.clear();
    if (manifest.browseLists.size() == 1) currentList = 0;  // its notice was already accepted
    startBrowse();
    return;
  }
  if (manifest.isXmlList() && !browseHistory.empty()) {
    browseCurrentUrl = browseHistory.back();
    browseHistory.pop_back();
    beginLoading();
    fetchXmlList();
  } else if (state == State::BROWSING && currentList >= 0 && manifest.browseLists.size() > 1) {
    // Browsing a picked list: Back returns to the list picker, not out (a
    // single list has no picker, so Back leaves the catalog).
    currentList = -1;
    startBrowse();
  } else {
    exitCatalog();
  }
}

void PluginCatalogActivity::activateIndex(const int index) {
  if (state == State::PLUGIN_PICKER) {
    if (index < 0 || index >= rowCount()) return;
    const PluginRef& plugin = installedPlugins[index];
    pickerReturnRow = index;
    if (plugin.manifestPath.empty()) {
      // Web-only: no catalog to open, so show its description + README (how
      // to use it from the web interface) instead.
      infoOpen = true;
      startActivityForResult(std::make_unique<PluginInfoActivity>(renderer, mappedInput, plugin),
                             [this](const ActivityResult&) {
                               infoOpen = false;
                               if (pickerReturnRow >= 0 && pickerReturnRow < rowCount())
                                 selectedIndex = pickerReturnRow;
                             });
      return;
    }
    manifestPath = plugin.manifestPath;
    catalogTitle = plugin.title;
    enterCatalog();
    return;
  }
  if (state == State::LIST_PICKER) {
    if (index < 0 || index >= static_cast<int>(manifest.browseLists.size())) return;
    openList(index);
    return;
  }
  // The pager rows bracket the items: "Previous page" ahead of them past
  // page 1, "Next page" after them while more pages exist.
  const int itemIndex = index - (prevRowVisible() ? 1 : 0);
  if (itemIndex == -1 || (nextRowVisible() && itemIndex == static_cast<int>(items.size()))) {
    beginLoading();
    fetchPage(itemIndex == -1 ? page - 1 : page + 1);
    return;
  }
  activateItem(itemIndex);
}

void PluginCatalogActivity::activateItem(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  const Item& item = items[itemIndex];
  if (manifest.isXmlList() && item.isDir) {
    browseHistory.push_back(browseCurrentUrl);
    browseCurrentUrl = item.url;
    beginLoading();
    fetchXmlList();
    return;
  }
  if (!manifest.isXmlList() && manifest.hasItemDetails()) {
    showItemDetails(itemIndex);
    return;
  }
  downloadItem(item);
}

// The selected row's detail fields, read back from the kept browse response
// with a filter admitting only those fields (and freed on return), then the
// optional details_url page. Every failure falls back to whatever was found;
// none of it blocks the download.
std::string PluginCatalogActivity::itemDetailsText(const Item& item) {
  std::string author, repository, description, detailsUrl, detailsFormat;
  if (item.sourceIndex >= 0) {
    JsonDocument filter;
    addFieldFilter(filter, manifest.itemsPath, manifest.detailAuthorPath);
    addFieldFilter(filter, manifest.itemsPath, manifest.repositoryPath);
    addFieldFilter(filter, manifest.itemsPath, manifest.descriptionPath);
    addFieldFilter(filter, manifest.itemsPath, manifest.detailsUrlPath);
    addFieldFilter(filter, manifest.itemsPath, manifest.detailsFormatPath);
    JsonDocument doc;
    HalFile file;
    if (Storage.openFileForRead("PCAT", BROWSE_TMP_PATH, file)) {
      struct HalFileReader {
        HalFile& f;
        int read() { return f.read(); }
        size_t readBytes(char* buf, size_t n) {
          const int r = f.read(buf, n);
          return r < 0 ? 0 : static_cast<size_t>(r);
        }
      } reader{file};
      if (deserializeJson(doc, reader, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
        const JsonVariantConst row =
            resolvePath(doc.as<JsonVariantConst>(), manifest.itemsPath).as<JsonArrayConst>()[item.sourceIndex];
        author = variantToString(resolvePath(row, manifest.detailAuthorPath));
        repository = variantToString(resolvePath(row, manifest.repositoryPath));
        description = variantToString(resolvePath(row, manifest.descriptionPath));
        detailsUrl = variantToString(resolvePath(row, manifest.detailsUrlPath));
        detailsFormat = variantToString(resolvePath(row, manifest.detailsFormatPath));
      } else {
        LOG_INF("PCAT", "item details unavailable (browse response unreadable)");
      }
    }
  }
  if (description.size() > MAX_ITEM_DESCRIPTION) description.resize(MAX_ITEM_DESCRIPTION);

  // A details page replaces the description when it arrives: GET, no headers
  // (it may be on another host), bounded, text or html.
  if (!detailsUrl.empty()) {
    std::string url = substituted(detailsUrl, &item);
    UrlUtils::resolveServerPath(url, substituted(activeBrowseUrl(), nullptr));
    std::string response;
    const int status = pluginhttp::request(session.get(), url, "GET", "", {}, response, MAX_ITEM_DETAILS_RESPONSE);
    const bool html = detailsFormat == "html";
    if (status < 200 || status >= 300 || response.empty()) {
      LOG_INF("PCAT", "item details fetch failed: HTTP %d", status);
    } else if (html) {
      description = htmlToPlainText(response);
    } else if (detailsFormat.empty() || detailsFormat == "text") {
      description = std::move(response);
    } else {
      LOG_INF("PCAT", "unsupported item details format: %s", detailsFormat.c_str());
    }
  }

  std::string text;
  text.reserve(description.size() + 160);
  const auto addLine = [&text](const std::string& line) {
    if (line.empty()) return;
    text += line;
    text += '\n';
  };
  if (!author.empty()) addLine(std::string(tr(STR_AUTHOR)) + ": " + author);
  if (!repository.empty()) addLine(std::string(tr(STR_REPOSITORY)) + ": " + repository);
  std::string facts;
  if (!item.version.empty()) facts = std::string(tr(STR_VERSION)) + " " + item.version;
  if (!item.status.empty() && item.status != "v" + item.version) {
    if (!facts.empty()) facts += " \xC2\xB7 ";
    facts += item.status;
  }
  addLine(facts);
  if (!description.empty()) {
    if (!text.empty()) text += '\n';
    text += description;
  }
  return text;
}

void PluginCatalogActivity::showItemDetails(const int itemIndex) {
  if (itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) return;
  beginLoading();  // the details page may be a network round trip
  const Item& item = items[itemIndex];
  std::string text = itemDetailsText(item);
  const bool update = item.status == tr(STR_UPDATE_AVAILABLE);
  const char* action = update ? tr(STR_UPDATE) : (manifest.tracksInstalls() ? tr(STR_INSTALL) : tr(STR_FETCH));
  state = State::BROWSING;  // the screen under the details
  infoOpen = true;
  startActivityForResult(
      std::make_unique<PluginInfoActivity>(renderer, mappedInput, item.title, std::move(text), action),
      [this, itemIndex](const ActivityResult& result) {
        infoOpen = false;
        if (result.isCancelled || itemIndex < 0 || itemIndex >= static_cast<int>(items.size())) {
          requestUpdate();
          return;
        }
        downloadItem(items[itemIndex]);
      });
}

// --- Rendering --------------------------------------------------------------

bool PluginCatalogActivity::rowsHaveSubtitle() const {
  if (state == State::PLUGIN_PICKER) return true;  // description / web-only hint
  if (state != State::BROWSING) return false;      // browse-list titles only
  for (const auto& item : items) {
    if (!item.author.empty()) return true;
  }
  return false;
}

Rect PluginCatalogActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // drawList takes the page-counter strip out of the rect it is handed.
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

std::string PluginCatalogActivity::browsingHeaderLabel() const {
  std::string label = catalogTitle;
  if (searchActive) {
    label = std::string(tr(STR_SEARCH)) + ": " + searchQuery;
  } else if (state == State::BROWSING && currentList >= 0 &&
             currentList < static_cast<int>(manifest.browseLists.size())) {
    label = manifest.browseLists[currentList].title;
  }
  return label;
}

void PluginCatalogActivity::drawListScreen() {
  const int total = rowCount();
  if (total == 0) {
    if (state == State::PLUGIN_PICKER && !SETTINGS.pluginsEnabled) {
      drawCenteredLines(nullptr, tr(STR_PLUGINS_DISABLED_HINT), nullptr);
      return;
    }
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                              state == State::PLUGIN_PICKER ? tr(STR_NO_PLUGINS_INSTALLED) : tr(STR_NO_ENTRIES));
    return;
  }

  const Rect rect = listRect();
  if (state == State::PLUGIN_PICKER) {
    GUI.drawList(
        renderer, rect, total, selectedIndex, [this](int i) -> std::string { return installedPlugins[i].title; },
        [this](int i) -> std::string {
          // A missing description falls back to the web-only hint so the row
          // still says what kind of plugin it is.
          const PluginRef& p = installedPlugins[i];
          if (!p.description.empty()) return p.description;
          return p.manifestPath.empty() ? std::string(tr(STR_PLUGIN_WEB_ONLY)) : std::string();
        });
    return;
  }
  if (state == State::LIST_PICKER) {
    GUI.drawList(renderer, rect, total, selectedIndex,
                 [this](int i) -> std::string { return manifest.browseLists[i].title; });
    return;
  }

  // BROWSING: pager rows bracket the items. These pages are the SERVER's, not
  // this rect's rows, so the counter strip gets our wording ("2 / 2+" while
  // the server says more exist); XML folders page by their own rows.
  const int prevOff = prevRowVisible() ? 1 : 0;
  const auto rowItem = [this, prevOff](const int i) -> const Item* {
    const int itemIndex = i - prevOff;
    return itemIndex >= 0 && itemIndex < static_cast<int>(items.size()) ? &items[itemIndex] : nullptr;
  };
  const bool hasSubtitle = rowsHaveSubtitle();
  char indicator[24] = {0};
  const char* indicatorOverride = nullptr;
  if (!manifest.isXmlList()) {
    snprintf(indicator, sizeof(indicator), hasMore ? "%d / %d+" : "%d / %d", page, page);
    indicatorOverride = indicator;
  }
  // Optional columns are empty std::functions when unused (drawList treats a
  // null callback as "no such column").
  std::function<std::string(int)> subtitle;
  if (hasSubtitle) {
    subtitle = [rowItem](int i) -> std::string {
      const Item* item = rowItem(i);
      return item ? item->author : std::string();
    };
  }
  // Install/update badge (plugin-store style catalogs); folders and pager
  // rows never carry one.
  std::function<std::string(int)> value;
  if (manifest.tracksInstalls()) {
    value = [rowItem](int i) -> std::string {
      const Item* item = rowItem(i);
      return item && !item->isDir ? item->status : std::string();
    };
  }
  GUI.drawList(
      renderer, rect, total, selectedIndex,
      [this, rowItem, prevOff](int i) -> std::string {
        if (const Item* item = rowItem(i)) return item->title;
        return std::string(i < prevOff ? tr(STR_PREV_PAGE) : tr(STR_NEXT_PAGE));
      },
      subtitle,
      [rowItem](int i) -> UIIcon {
        const Item* item = rowItem(i);
        if (!item) return UIIcon::Arrow;  // pager row
        return item->isDir ? UIIcon::Folder : UIIcon::File;
      },
      value, false, nullptr, indicatorOverride);
}

// Device-code sign-in: verification URL (text), the user code (bold) and a QR
// of the URL, then the polling notice.
void PluginCatalogActivity::drawAuthScreen() {
  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 6;
  const int codeH = renderer.getLineHeight(UI_12_FONT_ID) + 10;
  constexpr int qrSize = 180;
  constexpr int gap = 12;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int stackH = lineH + codeH + qrSize + gap + lineH;
  int y = std::max(top, (top + bottom) / 2 - stackH / 2);

  renderer.drawCenteredText(UI_10_FONT_ID, y,
                            renderer.truncatedText(UI_10_FONT_ID, authVerifyUrl.c_str(), pageWidth - 40).c_str());
  y += lineH;
  renderer.drawCenteredText(UI_12_FONT_ID, y, authUserCode.c_str(), true, EpdFontFamily::BOLD);
  y += codeH;
  QrUtils::drawQrCode(renderer, Rect{(pageWidth - qrSize) / 2, y, qrSize, qrSize}, authVerifyUrl);
  y += qrSize + gap;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_PLUGIN_AUTH_WAITING));
}

// Transfer screen: item title, the Downloading label and the running byte
// count (or a progress bar once the server has said how big the file is).
void PluginCatalogActivity::drawDownloadScreen() {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int lineH = 34;
  int y = pageHeight / 2 - lineH - lineH / 2;

  renderer.drawCenteredText(UI_10_FONT_ID, y,
                            renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40).c_str(), true,
                            EpdFontFamily::BOLD);
  y += lineH;
  renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_DOWNLOADING));
  y += lineH;
  char bytes[32];
  if (downloadProgress >= 1024 * 1024) {
    snprintf(bytes, sizeof(bytes), "%.1f MB", downloadProgress / (1024.0f * 1024.0f));
  } else {
    snprintf(bytes, sizeof(bytes), "%u KB", static_cast<unsigned>(downloadProgress / 1024));
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, bytes);
  y += lineH;
  if (downloadTotal > 0) {
    const int barW = pageWidth - 2 * metrics.contentSidePadding - 40;
    GUI.drawProgressBar(renderer, Rect{(pageWidth - barW) / 2, y, barW, metrics.progressBarHeight}, downloadProgress,
                        downloadTotal);
  }
}

// Centered message block: an optional bold heading, a wrapped body and an
// optional trailing hint line (the error, sign-in and done screens).
void PluginCatalogActivity::drawCenteredLines(const char* heading, const char* body, const char* hint) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, body ? body : "", pageWidth - 40, 6);
  const int count = (heading ? 1 : 0) + static_cast<int>(lines.size()) + (hint ? 1 : 0);
  int y = pageHeight / 2 - (count - 1) * lineH / 2;
  if (heading) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, heading, true, EpdFontFamily::BOLD);
    y += lineH;
  }
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lineH;
  }
  if (hint) renderer.drawCenteredText(SMALL_FONT_ID, y, hint);
}

void PluginCatalogActivity::drawFooter() {
  MappedInputManager::Labels labels;
  bool allSlots = false;
  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER: {
      const int count = rowCount();
      const int itemSel = selectedIndex - (prevRowVisible() ? 1 : 0);
      const char* confirmLabel;
      if (state != State::BROWSING) {
        // Every picker row opens something: a catalog, a browse list, or a
        // web-only plugin's info screen.
        confirmLabel = count > 0 ? tr(STR_OPEN) : "";
      } else {
        // Folders open; items and the pager rows both fetch from the server.
        const bool onItem = itemSel >= 0 && itemSel < static_cast<int>(items.size());
        const bool onDir = manifest.isXmlList() && onItem && items[itemSel].isDir;
        const bool onDetails = !manifest.isXmlList() && onItem && manifest.hasItemDetails();
        confirmLabel = count == 0 ? "" : (onDir ? tr(STR_OPEN) : (onDetails ? tr(STR_DETAILS) : tr(STR_FETCH)));
      }
      // On the top row of a searchable catalog the previous-nav slot becomes
      // Search; it is an action there, so the touch action bar draws it too.
      const bool searchable = state == State::BROWSING && manifest.hasSearch() && selectedIndex == 0;
      allSlots = searchable;
      const char* up = searchable ? tr(STR_SEARCH) : (count > 1 ? tr(STR_DIR_UP) : "");
      const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, up, down);
      break;
    }
    case State::ERROR:
    case State::NO_TOKEN: {
      const bool canSignIn = state == State::NO_TOKEN && manifest.hasDeviceCode();
      labels = mappedInput.mapLabels(tr(STR_BACK), canSignIn ? tr(STR_PLUGIN_SIGN_IN) : tr(STR_RETRY), "", "");
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    default:  // CHECK_WIFI / LOADING / AUTH / DONE (and child-activity handoffs)
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, allSlots);
}

void PluginCatalogActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // One header for every state, so it never shifts as the plugin moves
  // between them; the list states carry the list / search label.
  const std::string title = isListState() ? browsingHeaderLabel() : catalogTitle;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title.c_str());

  switch (state) {
    case State::BROWSING:
    case State::LIST_PICKER:
    case State::PLUGIN_PICKER:
      drawListScreen();
      break;
    case State::AUTH:
      drawAuthScreen();
      break;
    case State::DOWNLOADING:
      drawDownloadScreen();
      break;
    case State::DONE:
      drawCenteredLines(tr(STR_DOWNLOAD_COMPLETE), statusMessage.c_str(), nullptr);
      break;
    case State::ERROR:
      drawCenteredLines(tr(STR_ERROR_MSG), errorMessage.c_str(),
                        mappedInput.hasTouch() ? tr(STR_TAP_TO_RETRY) : nullptr);
      break;
    case State::NO_TOKEN:
      drawCenteredLines(nullptr, manifest.hasDeviceCode() ? tr(STR_PLUGIN_SIGN_IN_HINT) : tr(STR_PLUGIN_NOT_SIGNED_IN),
                        nullptr);
      break;
    default:  // CHECK_WIFI / LOADING (and the brief child-activity handoffs)
      renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, statusMessage.c_str());
      break;
  }

  drawFooter();
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}

#endif  // CROSSPOINT_SD_PLUGINS
