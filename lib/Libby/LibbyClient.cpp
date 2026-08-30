#include "LibbyClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <Util.h>  // freeink::content::base64Decode

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

using freeink::SecureHttpClient;

constexpr char SENTRY[] = "https://sentry.libbyapp.com";
// The `c` query on /chip, mirroring what the Libby web app sends.
constexpr char CLIENT[] = "d:22.1.0";
constexpr char ADOBE_EPUB_FORMAT[] = "ebook-epub-adobe";

constexpr char IDENTITY_PATH[] = "/.crosspoint/libby.json";
constexpr char CREDENTIAL_PATH[] = "/.crosspoint/content.key";
// /chip/sync is far too large for RAM; it lands here, is parsed back through a
// filter, and is deleted immediately. It is account metadata -- cards, holds,
// libraries -- and has no business persisting on the card.
constexpr char SYNC_TMP_PATH[] = "/.crosspoint/.libby-sync.tmp";
// Shared with the web UI, which writes the same shape: { "<loanId>": "<path>" }.
constexpr char BOOKS_PATH[] = "/.crosspoint/libby-books.json";

// OverDrive's public catalogue -- the same records the Libby web app paints
// from. Unauthenticated: no chip, no card, no Authorization header. Keyed on the
// title id, which is what a loan's `id` is.
constexpr char THUNDER_MEDIA[] = "https://thunder.api.overdrive.com/v2/media/";
// A media record carries the full blurb and every format, so it can run well
// past any sane in-RAM cap. Same treatment as /chip/sync: stream it to the card,
// filter it back, delete it.
constexpr char COVER_TMP_PATH[] = "/.crosspoint/.libby-cover.tmp";

// The chip is a JWT. 1KB is generous; a real one is ~600 bytes.
char g_identity[1280] = {};

SecureHttpClient* g_client = nullptr;

bool acquireClient() {
  if (g_client) return true;
  g_client = new (std::nothrow) SecureHttpClient();
  if (!g_client) return false;
  g_client->setUserAgent("CrossPoint");
  // SecureNet ships no CA bundle, so verification always fails (wolfSSL -188).
  // Traffic is still TLS-encrypted, just unauthenticated -- same posture as
  // HttpDownloader and the web relay.
  g_client->setInsecure();
  g_client->setTimeout(30000);
  return true;
}

void dropConnection() {
  if (!g_client) return;
  delete g_client;
  g_client = nullptr;
}

// Libby's Sentry API wants an Accept-Language derived from the chip: keep only
// a-z, reverse, take characters 5-6. Without it /chip/clone answers
// missing_chip. The identity-less call uses the same fixed seed as the web app.
void acceptLanguage(const char* chip, char* out, size_t outLen) {
  static constexpr char kSeed[] = "cudlkahllcnsjxhbmddl";
  const char* src = (chip && chip[0]) ? chip : kSeed;
  char letters[192];
  size_t n = 0;
  for (const char* p = src; *p && n < sizeof(letters); ++p) {
    if (*p >= 'a' && *p <= 'z') letters[n++] = *p;
  }
  // Reverse, then take [4,6).
  out[0] = '\0';
  if (n >= 6) {
    char rev[6];
    for (size_t i = 0; i < 6; i++) rev[i] = letters[n - 1 - i];
    snprintf(out, outLen, "%c%c", rev[4], rev[5]);
  }
}

// The `v` query on a chip refresh is the first segment of the chip UUID, which
// lives in the JWT payload as chip.id.
bool shortChipId(const char* chip, char* out, size_t outLen) {
  const char* first = strchr(chip, '.');
  if (!first) return false;
  const char* second = strchr(first + 1, '.');
  if (!second) return false;

  // base64url -> base64, into a bounded buffer.
  char payload[768];
  const size_t len = static_cast<size_t>(second - first - 1);
  if (len == 0 || len >= sizeof(payload)) return false;
  memcpy(payload, first + 1, len);
  payload[len] = '\0';
  for (char* p = payload; *p; ++p) {
    if (*p == '-')
      *p = '+';
    else if (*p == '_')
      *p = '/';
  }

  uint8_t decoded[768];
  const int32_t n = freeink::content::base64Decode(payload, strlen(payload), decoded, sizeof(decoded) - 1);
  if (n <= 0) return false;
  decoded[n] = 0;

  JsonDocument doc;
  if (deserializeJson(doc, reinterpret_cast<const char*>(decoded)) != DeserializationError::Ok) return false;
  const char* id = doc["chip"]["id"] | "";
  if (!id[0]) return false;

  // First dash-separated segment.
  size_t i = 0;
  while (id[i] && id[i] != '-' && i + 1 < outLen) {
    out[i] = id[i];
    i++;
  }
  out[i] = '\0';
  return i > 0;
}

// Adds the headers every Sentry call needs. `bearer` may be null for the
// identity-less mint call.
void applySentryHeaders(SecureHttpClient& http, const char* bearer) {
  char lang[8] = {};
  acceptLanguage(bearer, lang, sizeof(lang));
  http.addHeader("Accept", "application/json");
  http.addHeader("Referer", "https://libbyapp.com/");
  http.addHeader("Origin", "https://libbyapp.com");
  if (lang[0]) http.addHeader("Accept-Language", lang);
  if (bearer && bearer[0]) {
    std::string auth = "Bearer ";
    auth += bearer;
    http.addHeader("Authorization", auth);
  }
}

// A small JSON response accumulated in memory. Loan replies are a few hundred
// bytes; anything unexpectedly large is refused rather than grown into.
struct SmallResponse {
  static constexpr size_t LIMIT = 8 * 1024;
  std::string body;
  bool overflow = false;

  bool append(const uint8_t* data, size_t len) {
    if (body.size() + len > LIMIT) {
      overflow = true;
      return false;
    }
    body.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
};

// One Sentry request. Returns the HTTP status, or a negative value on transport
// failure. `jsonBody` may be null.
int sentry(const char* method, const char* path, const char* bearer, const char* jsonBody, SmallResponse* out) {
  if (!acquireClient()) return -1;

  char url[320];
  snprintf(url, sizeof(url), "%s%s", SENTRY, path);
  if (!g_client->begin(url)) {
    dropConnection();
    return -1;
  }
  applySentryHeaders(*g_client, bearer);
  if (jsonBody) g_client->addHeader("Content-Type", "application/json");

  const int status = g_client->sendRequest(
      method, reinterpret_cast<const uint8_t*>(jsonBody ? jsonBody : ""), jsonBody ? strlen(jsonBody) : 0,
      [&](const uint8_t* data, size_t len) { return out ? out->append(data, len) : true; });

  if (status < 0) dropConnection();
  return status;
}

// Persist a refreshed chip so later calls reuse it.
bool saveIdentity(const char* identity) {
  strlcpy(g_identity, identity, sizeof(g_identity));
  JsonDocument doc;
  doc["identity"] = g_identity;
  std::string text;
  serializeJson(doc, text);
  return Storage.writeFile(IDENTITY_PATH, String(text.c_str()));
}

// Loan endpoints can answer a valid chip with 403 missing_chip; re-registering
// and retrying clears it. Returns true when the chip was refreshed and the
// caller should retry.
bool refreshChip() {
  char shortId[40] = {};
  if (!shortChipId(g_identity, shortId, sizeof(shortId))) return false;

  char path[128];
  snprintf(path, sizeof(path), "/chip?c=%s&s=0&v=%s", CLIENT, shortId);
  SmallResponse resp;
  if (sentry("POST", path, g_identity, nullptr, &resp) != 200) return false;

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  const char* identity = doc["identity"] | "";
  if (!identity[0]) return false;

  LOG_DBG("LIBBY", "chip refreshed");
  return saveIdentity(identity);
}

// True when the reply is Libby's missing_chip refusal.
bool isMissingChip(int status, const SmallResponse& resp) {
  if (status != 403) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return false;
  const char* result = doc["result"] | "";
  return strcmp(result, "missing_chip") == 0;
}

// ISO-8601 (e.g. "2026-09-10T14:03:00Z") to epoch seconds. Returns 0 when the
// string is absent or unparseable, which the caller treats as "no due date".
//
// The arithmetic is done here rather than with timegm(): that is a GNU
// extension this tree relies on nowhere else, and mktime() would drag in the
// local timezone, which is wrong for a UTC timestamp and unset on this device
// anyway. This is Howard Hinnant's days-from-civil, which is exact for any
// date the Gregorian calendar covers.
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);            // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t isoToEpoch(const char* iso) {
  if (!iso || !iso[0]) return 0;
  int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) < 3) return 0;
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(mon), static_cast<unsigned>(day));
  return days * 86400 + hour * 3600 + min * 60 + sec;
}

// ArduinoJson reads from anything exposing read()/readBytes(). HalFile is a
// Print (output-only), so adapt it -- buffering the sync file in RAM instead
// would undo the entire reason it was streamed to the card.
class HalFileReader {
 public:
  explicit HalFileReader(HalFile& file) : file_(file) {}

  int read() { return file_.read(); }

  size_t readBytes(char* buffer, size_t length) {
    const int n = file_.read(buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

 private:
  HalFile& file_;
};

}  // namespace

const char* LibbyClient::errorText(const Error e) {
  switch (e) {
    case OK:
      return "OK";
    case NO_IDENTITY:
      return "No Libby account linked. Set it up in the web interface.";
    case NO_CREDENTIAL:
      return "This reader isn't authorised. Set it up in the web interface.";
    case NETWORK_ERROR:
      return "Couldn't reach Libby.";
    case AUTH_FAILED:
      return "Libby rejected this reader. Re-link the account in the web interface.";
    case SERVER_ERROR:
      return "Libby refused the request.";
    case JSON_ERROR:
      return "Libby sent something unreadable.";
    case NO_ADOBE_FORMAT:
      return "This title has no EPUB edition this reader can open.";
    case SD_ERROR:
      return "Couldn't write to the SD card.";
    case NOT_AUTHORISED:
      return "The library's distributor refused this reader.";
    case FULFIL_FAILED:
      return "The loan couldn't be prepared for this reader.";
  }
  return "Unknown error";
}

bool LibbyClient::hasIdentity() { return g_identity[0] != '\0'; }

bool LibbyClient::hasCredential() { return Storage.exists(CREDENTIAL_PATH); }

LibbyClient::Error LibbyClient::loadIdentity() {
  g_identity[0] = '\0';

  std::string text;
  if (!Storage.readFileToString("LIBBY", IDENTITY_PATH, 8 * 1024, text)) return NO_IDENTITY;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return JSON_ERROR;
  const char* identity = doc["identity"] | "";
  if (!identity[0]) return NO_IDENTITY;

  strlcpy(g_identity, identity, sizeof(g_identity));
  LOG_DBG("LIBBY", "identity loaded (%u bytes)", static_cast<unsigned>(strlen(g_identity)));
  return OK;
}

LibbyClient::Error LibbyClient::fetchLoans(LibbyLoanList& out) {
  out.count = 0;
  out.dropped = 0;
  if (!hasIdentity()) return NO_IDENTITY;
  if (!acquireClient()) return NETWORK_ERROR;

  // Stream the sync payload to the card. It carries every card, hold and
  // library the account touches -- tens of KB on a multi-card account -- so it
  // is never held in RAM.
  Storage.remove(SYNC_TMP_PATH);
  HalFile file;
  if (!Storage.openFileForWrite("LIBBY", SYNC_TMP_PATH, file)) return SD_ERROR;

  char url[192];
  snprintf(url, sizeof(url), "%s/chip/sync", SENTRY);
  if (!g_client->begin(url)) {
    file.close();
    Storage.remove(SYNC_TMP_PATH);
    dropConnection();
    return NETWORK_ERROR;
  }
  applySentryHeaders(*g_client, g_identity);

  bool writeFailed = false;
  const int status = g_client->GET([&](const uint8_t* data, size_t len) {
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    return true;
  });
  file.flush();
  file.close();

  if (status < 0) dropConnection();
  if (writeFailed) {
    Storage.remove(SYNC_TMP_PATH);
    return SD_ERROR;
  }
  if (status == 401 || status == 403) {
    Storage.remove(SYNC_TMP_PATH);
    return AUTH_FAILED;
  }
  if (status < 200 || status >= 300) {
    Storage.remove(SYNC_TMP_PATH);
    LOG_ERR("LIBBY", "sync failed: HTTP %d", status);
    return status < 0 ? NETWORK_ERROR : SERVER_ERROR;
  }

  // Parse it back through a filter so only the loan fields materialise; the
  // rest is skipped without ever being allocated.
  JsonDocument filter;
  JsonObject loanFilter = filter["loans"].add<JsonObject>();
  loanFilter["id"] = true;
  loanFilter["cardId"] = true;
  loanFilter["title"] = true;
  loanFilter["firstCreatorName"] = true;
  loanFilter["expireDate"] = true;
  loanFilter["formats"].add<JsonObject>()["id"] = true;

  HalFile readBack;
  if (!Storage.openFileForRead("LIBBY", SYNC_TMP_PATH, readBack)) {
    Storage.remove(SYNC_TMP_PATH);
    return SD_ERROR;
  }
  JsonDocument doc;
  HalFileReader reader(readBack);
  const DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  readBack.close();
  Storage.remove(SYNC_TMP_PATH);  // account metadata: do not leave it lying around

  if (err != DeserializationError::Ok) {
    LOG_ERR("LIBBY", "sync parse failed: %s", err.c_str());
    return JSON_ERROR;
  }

  for (JsonObject loan : doc["loans"].as<JsonArray>()) {
    if (out.count >= LibbyLoanList::MAX_LOANS) {
      out.dropped++;
      continue;
    }
    LibbyLoan& dst = out.loans[out.count];
    strlcpy(dst.id, loan["id"] | "", sizeof(dst.id));
    strlcpy(dst.cardId, loan["cardId"] | "", sizeof(dst.cardId));
    strlcpy(dst.title, loan["title"] | "", sizeof(dst.title));
    strlcpy(dst.author, loan["firstCreatorName"] | "", sizeof(dst.author));
    dst.expiresAt = isoToEpoch(loan["expireDate"] | "");

    // Prefer the exact Adobe EPUB id; accept any epub+adobe variant.
    dst.format[0] = '\0';
    for (JsonObject fmt : loan["formats"].as<JsonArray>()) {
      const char* id = fmt["id"] | "";
      if (strcmp(id, ADOBE_EPUB_FORMAT) == 0) {
        strlcpy(dst.format, id, sizeof(dst.format));
        break;
      }
      if (!dst.format[0] && strstr(id, "epub") && strstr(id, "adobe")) {
        strlcpy(dst.format, id, sizeof(dst.format));
      }
    }

    if (dst.id[0]) out.count++;
  }

  LOG_INF("LIBBY", "%d loans (%d dropped)", out.count, out.dropped);
  return OK;
}

LibbyClient::Error LibbyClient::fetchAcsm(const LibbyLoan& loan, const char* acsmPath) {
  if (!hasIdentity()) return NO_IDENTITY;
  if (!loan.sendable()) return NO_ADOBE_FORMAT;

  char path[192];
  snprintf(path, sizeof(path), "/card/%s/loan/%s/fulfill/%s", loan.cardId, loan.id, loan.format);

  SmallResponse resp;
  int status = sentry("GET", path, g_identity, nullptr, &resp);
  if (isMissingChip(status, resp) && refreshChip()) {
    resp.body.clear();
    status = sentry("GET", path, g_identity, nullptr, &resp);
  }
  if (status < 0) return NETWORK_ERROR;
  if (status == 401 || status == 403) return AUTH_FAILED;
  if (status != 200) {
    LOG_ERR("LIBBY", "fulfil request failed: HTTP %d", status);
    return SERVER_ERROR;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return JSON_ERROR;
  const char* href = doc["fulfill"]["href"] | "";
  if (!href[0]) {
    LOG_ERR("LIBBY", "fulfil reply carried no href");
    return JSON_ERROR;
  }

  // The href points at the actual token. Small (a few KB of XML), so it is
  // fetched into memory and written out in one go.
  if (!acquireClient()) return NETWORK_ERROR;
  if (!g_client->begin(href)) {
    dropConnection();
    return NETWORK_ERROR;
  }
  g_client->setFollowRedirects(5);
  g_client->addHeader("Accept", "*/*");
  SmallResponse token;
  const int tokenStatus = g_client->GET([&](const uint8_t* data, size_t len) { return token.append(data, len); });
  if (tokenStatus < 0) {
    dropConnection();
    return NETWORK_ERROR;
  }
  if (tokenStatus != 200 || token.body.empty()) {
    LOG_ERR("LIBBY", "token download failed: HTTP %d%s", tokenStatus, token.overflow ? " (too large)" : "");
    return SERVER_ERROR;
  }

  HalFile out;
  if (!Storage.openFileForWrite("LIBBY", acsmPath, out)) return SD_ERROR;
  const size_t written = out.write(token.body.data(), token.body.size());
  out.flush();
  out.close();
  if (written != token.body.size()) {
    Storage.remove(acsmPath);
    return SD_ERROR;
  }

  LOG_INF("LIBBY", "fulfilment token saved (%u bytes)", static_cast<unsigned>(token.body.size()));
  return OK;
}

namespace {
// Renew and return differ only by HTTP method, and share the missing_chip retry.
LibbyClient::Error loanAction(const LibbyLoan& loan, const char* method, const char* body) {
  if (!LibbyClient::hasIdentity()) return LibbyClient::NO_IDENTITY;

  char path[128];
  snprintf(path, sizeof(path), "/card/%s/loan/%s", loan.cardId, loan.id);

  SmallResponse resp;
  int status = sentry(method, path, g_identity, body, &resp);
  if (isMissingChip(status, resp) && refreshChip()) {
    resp.body.clear();
    status = sentry(method, path, g_identity, body, &resp);
  }
  if (status < 0) return LibbyClient::NETWORK_ERROR;
  if (status == 401) return LibbyClient::AUTH_FAILED;
  if (status < 200 || status >= 300) {
    LOG_ERR("LIBBY", "%s loan failed: HTTP %d", method, status);
    return LibbyClient::SERVER_ERROR;
  }
  return LibbyClient::OK;
}
}  // namespace

LibbyClient::Error LibbyClient::renewLoan(const LibbyLoan& loan) { return loanAction(loan, "PUT", "{}"); }

LibbyClient::Error LibbyClient::returnLoan(const LibbyLoan& loan) { return loanAction(loan, "DELETE", nullptr); }

bool LibbyClient::rememberBook(const char* loanId, const char* bookPath) {
  if (!loanId || !loanId[0] || !bookPath || !bookPath[0]) return false;

  // Read-modify-write. The map holds one short string per loan a borrowing
  // limit allows, so it stays a few hundred bytes.
  JsonDocument doc;
  std::string existing;
  if (Storage.readFileToString("LIBBY", BOOKS_PATH, 16 * 1024, existing)) {
    if (deserializeJson(doc, existing) != DeserializationError::Ok) doc.clear();
  }
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  doc[loanId] = bookPath;

  std::string text;
  serializeJson(doc, text);
  return Storage.writeFile(BOOKS_PATH, String(text.c_str()));
}

bool LibbyClient::lookupBook(const char* loanId, char* outPath, const size_t outLen) {
  if (outPath && outLen) outPath[0] = '\0';
  if (!loanId || !loanId[0]) return false;

  std::string text;
  if (!Storage.readFileToString("LIBBY", BOOKS_PATH, 16 * 1024, text)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return false;
  const char* path = doc[loanId] | "";
  if (!path[0]) return false;

  // A book deleted from the card leaves a stale entry; treat that as "no copy"
  // rather than trying to rewrite a sidecar for a file that is gone.
  if (!Storage.exists(path)) {
    LOG_DBG("LIBBY", "mapped book missing from card: %s", path);
    return false;
  }
  strlcpy(outPath, path, outLen);
  return true;
}

bool LibbyClient::fetchCoverUrl(const LibbyLoan& loan, char* outUrl, const size_t outLen) {
  if (!outUrl || outLen == 0) return false;
  outUrl[0] = '\0';

  // The id is spliced into a URL path. Real title ids are digits, but this one
  // arrived in a JSON reply, so refuse anything that isn't path-safe rather
  // than trusting it.
  if (!loan.id[0]) return false;
  for (const char* p = loan.id; *p; ++p) {
    if (!isalnum(static_cast<unsigned char>(*p)) && *p != '-') {
      LOG_ERR("LIBBY", "loan id is not usable in a URL path");
      return false;
    }
  }

  if (!acquireClient()) return false;

  Storage.remove(COVER_TMP_PATH);
  HalFile file;
  if (!Storage.openFileForWrite("LIBBY", COVER_TMP_PATH, file)) return false;

  char url[192];
  snprintf(url, sizeof(url), "%s%s", THUNDER_MEDIA, loan.id);
  if (!g_client->begin(url)) {
    file.close();
    Storage.remove(COVER_TMP_PATH);
    dropConnection();
    return false;
  }
  // Deliberately no applySentryHeaders(): this is the catalogue, not Libby, and
  // sending the chip to it would leak the account for no benefit.
  g_client->addHeader("Accept", "application/json");

  bool writeFailed = false;
  const int status = g_client->GET([&](const uint8_t* data, size_t len) {
    if (file.write(data, len) != len) {
      writeFailed = true;
      return false;
    }
    return true;
  });
  file.flush();
  file.close();

  if (status < 0) dropConnection();
  if (writeFailed || status < 200 || status >= 300) {
    Storage.remove(COVER_TMP_PATH);
    LOG_ERR("LIBBY", "cover lookup for %s failed: HTTP %d%s", loan.id, status, writeFailed ? " (SD write)" : "");
    return false;
  }

  // Keep only the covers block; the blurb and the format list never materialise.
  JsonDocument filter;
  filter["covers"] = true;

  HalFile readBack;
  if (!Storage.openFileForRead("LIBBY", COVER_TMP_PATH, readBack)) {
    Storage.remove(COVER_TMP_PATH);
    return false;
  }
  JsonDocument doc;
  HalFileReader reader(readBack);
  const DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  readBack.close();
  Storage.remove(COVER_TMP_PATH);

  if (err != DeserializationError::Ok) {
    LOG_ERR("LIBBY", "cover lookup parse failed: %s", err.c_str());
    return false;
  }

  // Widest wins. OverDrive names the entries cover150Wide/cover300Wide/
  // cover510Wide, but which of them a title carries varies, so choose on the
  // declared width rather than on a key that may not be there. The extra pixels
  // are worth having: these are progressive JPEGs, and the decoder in this
  // firmware only reads their DC scan, so whatever arrives is downscaled to an
  // eighth before it reaches the screen.
  int bestWidth = 0;
  for (JsonPair entry : doc["covers"].as<JsonObject>()) {
    JsonObject cover = entry.value().as<JsonObject>();
    const char* href = cover["href"] | "";
    const int width = cover["width"] | 0;
    if (!href[0] || width <= bestWidth) continue;
    if (cover["isPlaceholderImage"] | false) continue;  // a generic "no artwork" tile
    bestWidth = width;
    strlcpy(outUrl, href, outLen);
  }

  if (!outUrl[0]) {
    LOG_DBG("LIBBY", "no cover artwork published for %s", loan.id);
    return false;
  }
  LOG_INF("LIBBY", "cover %dpx wide for %s", bestWidth, loan.id);
  return true;
}

void LibbyClient::closeConnection() { dropConnection(); }
