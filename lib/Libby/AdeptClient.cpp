#include "AdeptClient.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <Util.h>
#include <WolfsslCrypto.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using freeink::SecureHttpClient;
using freeink::content::WolfsslCrypto;

constexpr char ADEPT_NS[] = "http://ns.adobe.com/adept";
constexpr char ADEPT_CT[] = "application/vnd.adobe.adept+xml";
constexpr char SESSION_PATH[] = "/.crosspoint/libby-session.json";

// ---------------------------------------------------------------------------
// session (written by the web UI)
// ---------------------------------------------------------------------------

struct AdeptSession {
  std::string userUuid;
  std::string deviceUuid;
  std::string deviceType;
  std::string signingKey;   // PKCS#8 DER, base64
  std::string signingCert;  // DER, base64
  std::string licenseCert;  // DER, base64
  std::string activationUrl;
  std::string authUrl;
  // Fetched at use time rather than stored: the service hands them out freely
  // and keeping them would have bloated the credential file.
  std::string authenticationCert;
  // Operators already authenticated this session, so the handshake is not
  // repeated per book.
  std::vector<std::string> authedOperators;
  bool loaded = false;
};

AdeptSession g_session;
SecureHttpClient* g_client = nullptr;

WolfsslCrypto& crypto() {
  static WolfsslCrypto instance;
  return instance;
}

// ---------------------------------------------------------------------------
// base64 (Util.h ships a decoder only)
// ---------------------------------------------------------------------------

std::string base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out += kAlphabet[(triple >> 18) & 0x3f];
    out += kAlphabet[(triple >> 12) & 0x3f];
    out += (i + 1 < len) ? kAlphabet[(triple >> 6) & 0x3f] : '=';
    out += (i + 2 < len) ? kAlphabet[triple & 0x3f] : '=';
  }
  return out;
}

// ---------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------
//
// A deliberately small model: ADEPT documents are shallow, and an element
// carries either text or child elements, never both interleaved. Storing text
// separately from children costs nothing here and keeps the canonicaliser
// simple. If a document ever did mix them, the canonicalisation below would
// emit the text first rather than in document order -- noted because that would
// invalidate a signature, loudly rather than subtly.

struct XmlNode {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<XmlNode> kids;
  std::string text;

  const std::string* attr(const std::string& key) const {
    for (const auto& a : attrs) {
      if (a.first == key) return &a.second;
    }
    return nullptr;
  }
};

// Local name comparison: ADEPT mixes prefixed and unprefixed forms freely.
bool named(const XmlNode& n, const char* local) {
  if (n.name == local) return true;
  const size_t colon = n.name.find(':');
  return colon != std::string::npos && n.name.compare(colon + 1, std::string::npos, local) == 0;
}

XmlNode* findChild(XmlNode& n, const char* local) {
  for (auto& k : n.kids) {
    if (named(k, local)) return &k;
  }
  return nullptr;
}

XmlNode* findDeep(XmlNode& n, const char* local) {
  if (named(n, local)) return &n;
  for (auto& k : n.kids) {
    if (XmlNode* hit = findDeep(k, local)) return hit;
  }
  return nullptr;
}

std::string textOf(const XmlNode& n) {
  if (!n.text.empty()) return n.text;
  std::string out;
  for (const auto& k : n.kids) out += textOf(k);
  return out;
}

void escapeInto(std::string& out, const std::string& in, bool attribute) {
  for (const char c : in) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += attribute ? "&quot;" : "\"";
        break;
      default:
        out += c;
    }
  }
}

void serializeInto(std::string& out, const XmlNode& n) {
  out += '<';
  out += n.name;
  for (const auto& a : n.attrs) {
    out += ' ';
    out += a.first;
    out += "=\"";
    escapeInto(out, a.second, true);
    out += '"';
  }
  if (n.kids.empty() && n.text.empty()) {
    out += "/>";
    return;
  }
  out += '>';
  if (!n.text.empty()) escapeInto(out, n.text, false);
  for (const auto& k : n.kids) serializeInto(out, k);
  out += "</";
  out += n.name;
  out += '>';
}

std::string serialize(const XmlNode& n) {
  std::string out = "<?xml version=\"1.0\"?>\n";
  serializeInto(out, n);
  return out;
}

std::string decodeEntities(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '&') {
      out += in[i++];
      continue;
    }
    const size_t semi = in.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out += in[i++];
      continue;
    }
    const std::string ent = in.substr(i + 1, semi - i - 1);
    if (ent == "amp")
      out += '&';
    else if (ent == "lt")
      out += '<';
    else if (ent == "gt")
      out += '>';
    else if (ent == "quot")
      out += '"';
    else if (ent == "apos")
      out += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      const long code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? strtol(ent.c_str() + 2, nullptr, 16)
                                                                             : strtol(ent.c_str() + 1, nullptr, 10);
      if (code > 0 && code < 128) out += static_cast<char>(code);
    } else {
      out += in.substr(i, semi - i + 1);
    }
    i = semi + 1;
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && isspace(static_cast<unsigned char>(s[b]))) b++;
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

// Finds the '>' that closes a tag, ignoring quoted attribute values.
size_t findTagEnd(const std::string& src, size_t start) {
  char quote = 0;
  for (size_t i = start + 1; i < src.size(); i++) {
    const char c = src[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i;
    }
  }
  return std::string::npos;
}

void parseTag(const std::string& content, XmlNode& out) {
  size_t i = 0;
  while (i < content.size() && !isspace(static_cast<unsigned char>(content[i])) && content[i] != '/') i++;
  out.name = content.substr(0, i);
  while (i < content.size()) {
    while (i < content.size() && isspace(static_cast<unsigned char>(content[i]))) i++;
    const size_t nameStart = i;
    while (i < content.size() && content[i] != '=' && !isspace(static_cast<unsigned char>(content[i]))) i++;
    if (i >= content.size() || nameStart == i) break;
    const std::string key = content.substr(nameStart, i - nameStart);
    while (i < content.size() && isspace(static_cast<unsigned char>(content[i]))) i++;
    if (i >= content.size() || content[i] != '=') continue;
    i++;
    while (i < content.size() && isspace(static_cast<unsigned char>(content[i]))) i++;
    if (i >= content.size()) break;
    const char quote = content[i];
    if (quote != '"' && quote != '\'') continue;
    const size_t valStart = ++i;
    while (i < content.size() && content[i] != quote) i++;
    out.attrs.emplace_back(key, decodeEntities(content.substr(valStart, i - valStart)));
    if (i < content.size()) i++;
  }
}

// Small recursive-descent XML parser. Mirrors the browser implementation so the
// two produce identical trees -- which matters, because both sign them.
bool parseXml(const std::string& src, XmlNode& root) {
  // The open-element stack is a path of CHILD INDICES, not pointers. Pointers
  // would dangle: push_back() into a kids vector can reallocate it, and every
  // stacked pointer into that vector then points at freed memory. Re-walking
  // the path is O(depth), and ADEPT documents are a handful of levels deep.
  std::vector<size_t> path;
  const auto current = [&]() -> XmlNode* {
    XmlNode* n = &root;
    for (const size_t idx : path) n = &n->kids[idx];
    return n;
  };
  bool haveRoot = false;
  size_t i = 0;

  while (i < src.size()) {
    if (src[i] != '<') {
      const size_t next = src.find('<', i);
      const std::string raw = src.substr(i, (next == std::string::npos ? src.size() : next) - i);
      const std::string t = trim(raw);
      if (!t.empty() && haveRoot) current()->text += decodeEntities(t);
      i = (next == std::string::npos) ? src.size() : next;
      continue;
    }
    if (src.compare(i, 2, "<?") == 0) {
      const size_t end = src.find("?>", i);
      if (end == std::string::npos) return false;
      i = end + 2;
      continue;
    }
    if (src.compare(i, 4, "<!--") == 0) {
      const size_t end = src.find("-->", i);
      if (end == std::string::npos) return false;
      i = end + 3;
      continue;
    }
    if (src.compare(i, 9, "<![CDATA[") == 0) {
      const size_t end = src.find("]]>", i);
      if (end == std::string::npos) return false;
      if (haveRoot) current()->text += src.substr(i + 9, end - i - 9);
      i = end + 3;
      continue;
    }
    if (src.compare(i, 2, "<!") == 0) {
      const size_t end = src.find('>', i);
      if (end == std::string::npos) return false;
      i = end + 1;
      continue;
    }
    if (src[i + 1] == '/') {
      const size_t end = src.find('>', i);
      if (end == std::string::npos || path.empty()) return false;
      path.pop_back();
      i = end + 1;
      continue;
    }

    const size_t end = findTagEnd(src, i);
    if (end == std::string::npos) return false;
    std::string content = trim(src.substr(i + 1, end - i - 1));
    bool selfClose = false;
    if (!content.empty() && content.back() == '/') {
      selfClose = true;
      content.pop_back();
      content = trim(content);
    }

    XmlNode node;
    parseTag(content, node);
    if (node.name.empty()) return false;

    if (!haveRoot) {
      if (!path.empty()) return false;
      root = std::move(node);
      haveRoot = true;
      // The root is not pushed: an empty path already denotes it.
    } else {
      XmlNode* parent = current();
      parent->kids.push_back(std::move(node));
      if (!selfClose) path.push_back(parent->kids.size() - 1);
    }
    i = end + 1;
  }
  return haveRoot && path.empty();
}

// ---------------------------------------------------------------------------
// request signing
// ---------------------------------------------------------------------------
//
// ADEPT signs a SHA-1 over its own binary serialisation of the request tree:
// namespace prefixes resolved to URIs, attributes sorted, text trimmed. It must
// match byte for byte or the server rejects the signature, so this is a
// transcription of that format and not any standard canonical XML.

constexpr uint8_t ASN_NS_TAG = 0x01;
constexpr uint8_t ASN_CHILD = 0x02;
constexpr uint8_t ASN_END_TAG = 0x03;
constexpr uint8_t ASN_TEXT = 0x04;
constexpr uint8_t ASN_ATTRIBUTE = 0x05;

void pushString(std::string& out, const std::string& s) {
  out += static_cast<char>((s.size() >> 8) & 0xff);  // uint16, big endian
  out += static_cast<char>(s.size() & 0xff);
  out += s;
}

void canonWalk(std::string& out, const XmlNode& n, std::vector<std::pair<std::string, std::string>> ns) {
  for (const auto& a : n.attrs) {
    if (a.first == "xmlns")
      ns.emplace_back("GENERICNS", a.second);
    else if (a.first.compare(0, 6, "xmlns:") == 0)
      ns.emplace_back(a.first.substr(6), a.second);
  }
  const auto lookup = [&ns](const std::string& prefix) -> const std::string* {
    for (auto it = ns.rbegin(); it != ns.rend(); ++it) {
      if (it->first == prefix) return &it->second;
    }
    return nullptr;
  };

  std::string local = n.name;
  const size_t colon = local.find(':');
  if (colon != std::string::npos) {
    out += static_cast<char>(ASN_NS_TAG);
    const std::string* uri = lookup(local.substr(0, colon));
    pushString(out, uri ? *uri : std::string());
    local = local.substr(colon + 1);
  } else if (const std::string* generic = lookup("GENERICNS")) {
    out += static_cast<char>(ASN_NS_TAG);
    pushString(out, *generic);
  }
  pushString(out, local);

  std::vector<const std::pair<std::string, std::string>*> sorted;
  for (const auto& a : n.attrs) {
    if (a.first.find("xmlns") == std::string::npos) sorted.push_back(&a);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const std::pair<std::string, std::string>* a, const std::pair<std::string, std::string>* b) {
              return a->first < b->first;
            });
  for (const auto* a : sorted) {
    out += static_cast<char>(ASN_ATTRIBUTE);
    pushString(out, std::string());
    pushString(out, a->first);
    pushString(out, a->second);
  }

  out += static_cast<char>(ASN_CHILD);
  const std::string t = trim(n.text);
  if (!t.empty()) {
    out += static_cast<char>(ASN_TEXT);
    pushString(out, t);
  }
  for (const auto& k : n.kids) canonWalk(out, k, ns);
  out += static_cast<char>(ASN_END_TAG);
}

bool signNode(XmlNode& root) {
  if (g_session.signingKey.empty()) return false;

  std::string canon;
  canonWalk(canon, root, {});

  uint8_t hash[20];
  crypto().sha1(reinterpret_cast<const uint8_t*>(canon.data()), canon.size(), hash);

  const std::string keyDer = freeink::content::base64Decode(g_session.signingKey);
  if (keyDer.empty()) return false;

  uint8_t sig[128];
  if (!crypto().rsaPrivateSignRaw(reinterpret_cast<const uint8_t*>(keyDer.data()), keyDer.size(), hash, sig)) {
    LOG_ERR("ADEPT", "request signing failed: %s", crypto().lastError.c_str());
    return false;
  }

  XmlNode signature;
  signature.name = "adept:signature";
  signature.text = base64Encode(sig, sizeof(sig));
  root.kids.push_back(std::move(signature));
  return true;
}

// The nonce is a 12-byte counter seeded from the clock; the server only checks
// it for replay. The clock is NTP-set at boot -- an unsynced device will have
// its requests rejected as expired, which is the correct outcome.
void addNonce(XmlNode& root) {
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  uint8_t buf[12] = {};
  const uint32_t low = static_cast<uint32_t>(now & 0xffffffffLL);
  const uint32_t high = static_cast<uint32_t>((now >> 32) & 0xffffffffLL);
  const uint32_t a = 0x6f046000u + low;
  const uint32_t b = 0x388au + high;
  memcpy(buf, &a, 4);
  memcpy(buf + 4, &b, 4);

  XmlNode nonce;
  nonce.name = "adept:nonce";
  nonce.text = base64Encode(buf, sizeof(buf));
  root.kids.push_back(std::move(nonce));

  const time_t expiry = time(nullptr) + 10 * 60;
  struct tm tmv;
  gmtime_r(&expiry, &tmv);
  char iso[32];
  strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmv);

  XmlNode exp;
  exp.name = "adept:expiration";
  exp.text = iso;
  root.kids.push_back(std::move(exp));
}

XmlNode textElement(const char* name, const std::string& value) {
  XmlNode n;
  n.name = name;
  n.text = value;
  return n;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

bool acquireClient() {
  if (g_client) return true;
  g_client = new (std::nothrow) SecureHttpClient();
  if (!g_client) return false;
  g_client->setUserAgent("CrossPoint");
  g_client->setInsecure();  // SecureNet ships no CA bundle; see LibbyClient
  g_client->setTimeout(60000);
  g_client->setFollowRedirects(5);
  return true;
}

void dropClient() {
  delete g_client;
  g_client = nullptr;
}

// An ADEPT exchange is a few KB each way. Anything larger is refused rather
// than grown into.
constexpr size_t ADEPT_BODY_LIMIT = 24 * 1024;

int httpExchange(const char* method, const std::string& url, const char* contentType, const std::string& body,
                 std::string& out) {
  if (!acquireClient()) return -1;
  out.clear();
  if (!g_client->begin(url)) {
    dropClient();
    return -1;
  }
  if (contentType) g_client->addHeader("Content-Type", contentType);
  g_client->addHeader("Accept", "*/*");

  bool tooLarge = false;
  const int status = g_client->sendRequest(method, reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                           [&](const uint8_t* data, size_t len) {
                                             if (out.size() + len > ADEPT_BODY_LIMIT) {
                                               tooLarge = true;
                                               return false;
                                             }
                                             out.append(reinterpret_cast<const char*>(data), len);
                                             return true;
                                           });
  if (status < 0) dropClient();
  if (tooLarge) LOG_ERR("ADEPT", "reply exceeded %u bytes", static_cast<unsigned>(ADEPT_BODY_LIMIT));
  return status;
}

// POST a signed ADEPT document and parse the reply. `err` is set to the
// service's own message when it answers <error>.
AdeptClient::Error sendAdept(const std::string& url, const XmlNode& doc, XmlNode& reply, std::string& err) {
  std::string body;
  const int status = httpExchange("POST", url, ADEPT_CT, serialize(doc), body);
  if (status < 0) return AdeptClient::NETWORK_ERROR;

  if (!parseXml(body, reply)) {
    LOG_ERR("ADEPT", "unparseable reply (HTTP %d) from %s", status, url.c_str());
    return status >= 200 && status < 300 ? AdeptClient::SERVER_ERROR : AdeptClient::NETWORK_ERROR;
  }
  if (named(reply, "error")) {
    const std::string* data = reply.attr("data");
    err = data ? *data : "ADEPT error";
    LOG_ERR("ADEPT", "service error: %s", err.c_str());
    return err.find("E_ADEPT_DISTRIBUTOR_AUTH") != std::string::npos ? AdeptClient::DISTRIBUTOR_AUTH
                                                                     : AdeptClient::SERVER_ERROR;
  }
  if (status < 200 || status >= 300) return AdeptClient::SERVER_ERROR;
  return AdeptClient::OK;
}

AdeptClient::Error httpGetXml(const std::string& url, XmlNode& out) {
  std::string body;
  const int status = httpExchange("GET", url, nullptr, std::string(), body);
  if (status < 0) return AdeptClient::NETWORK_ERROR;
  if (status < 200 || status >= 300) return AdeptClient::SERVER_ERROR;
  return parseXml(body, out) ? AdeptClient::OK : AdeptClient::SERVER_ERROR;
}

// The authentication certificate is not persisted (it is public and freely
// served), so fetch it the first time a fulfilment needs it.
AdeptClient::Error ensureAuthCertificate() {
  if (!g_session.authenticationCert.empty()) return AdeptClient::OK;
  if (g_session.authUrl.empty()) return AdeptClient::NOT_SET_UP;

  XmlNode info;
  const AdeptClient::Error e = httpGetXml(g_session.authUrl + "/AuthenticationServiceInfo", info);
  if (e != AdeptClient::OK) return e;
  XmlNode* cert = findDeep(info, "certificate");
  if (!cert) return AdeptClient::SERVER_ERROR;
  g_session.authenticationCert = trim(textOf(*cert));
  return g_session.authenticationCert.empty() ? AdeptClient::SERVER_ERROR : AdeptClient::OK;
}

std::string operatorBase(const std::string& fulfilUrl) {
  std::string base = fulfilUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  static constexpr char kSuffix[] = "/Fulfill";
  const size_t n = sizeof(kSuffix) - 1;
  if (base.size() > n && base.compare(base.size() - n, n, kSuffix) == 0) base.resize(base.size() - n);
  return base;
}

// Introduce this reader to a distributor it has not dealt with before.
AdeptClient::Error operatorAuth(const std::string& fulfilUrl, std::string& err) {
  for (const auto& seen : g_session.authedOperators) {
    if (seen == fulfilUrl) return AdeptClient::OK;
  }
  const AdeptClient::Error certErr = ensureAuthCertificate();
  if (certErr != AdeptClient::OK) return certErr;

  const std::string base = operatorBase(fulfilUrl);

  XmlNode creds;
  creds.name = "adept:credentials";
  creds.attrs.emplace_back("xmlns:adept", ADEPT_NS);
  creds.kids.push_back(textElement("adept:user", g_session.userUuid));
  creds.kids.push_back(textElement("adept:certificate", g_session.signingCert));
  creds.kids.push_back(textElement("adept:licenseCertificate", g_session.licenseCert));
  creds.kids.push_back(textElement("adept:authenticationCertificate", g_session.authenticationCert));

  XmlNode reply;
  AdeptClient::Error e = sendAdept(base + "/Auth", creds, reply, err);
  if (e != AdeptClient::OK) return e;

  XmlNode init;
  init.name = "adept:licenseServiceRequest";
  init.attrs.emplace_back("xmlns:adept", ADEPT_NS);
  init.attrs.emplace_back("identity", "user");
  init.kids.push_back(textElement("adept:operatorURL", base));
  addNonce(init);
  init.kids.push_back(textElement("adept:user", g_session.userUuid));
  if (!signNode(init)) return AdeptClient::SIGN_FAILED;

  XmlNode initReply;
  e = sendAdept(g_session.activationUrl + "/InitLicenseService", init, initReply, err);
  if (e != AdeptClient::OK) return e;

  g_session.authedOperators.push_back(fulfilUrl);
  return AdeptClient::OK;
}

// Strip characters a FAT filesystem (or a reader) will not thank us for.
void sanitiseTitle(const std::string& in, char* out, size_t outLen) {
  std::string clean;
  clean.reserve(in.size());
  for (const char c : in) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f)
      clean += ' ';
    else if (strchr("/\\:*?\"<>|", c))
      clean += ' ';
    else
      clean += c;
  }
  clean = trim(clean);
  while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());
  while (!clean.empty() && clean.back() == '.') clean.pop_back();
  if (clean.size() > 60) clean.resize(60);
  clean = trim(clean);
  strlcpy(out, clean.empty() ? "book" : clean.c_str(), outLen);
}

bool uniqueDestination(const char* destDir, const char* title, char* out, size_t outLen) {
  for (int n = 1; n < 1000; n++) {
    if (n == 1)
      snprintf(out, outLen, "%s/%s.epub", destDir, title);
    else
      snprintf(out, outLen, "%s/%s (%d).epub", destDir, title, n);
    if (!Storage.exists(out)) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

const char* AdeptClient::errorText(const Error e) {
  switch (e) {
    case OK:
      return "OK";
    case NOT_SET_UP:
      return "This reader isn't authorised. Set it up in the web interface.";
    case BAD_TOKEN:
      return "Libby returned something that isn't a loan token.";
    case NETWORK_ERROR:
      return "Couldn't reach the library's servers.";
    case DISTRIBUTOR_AUTH:
      return "The library's distributor refused this reader.";
    case SERVER_ERROR:
      return "The library's servers refused the loan.";
    case SIGN_FAILED:
      return "This reader's authorisation is incomplete. Set it up again.";
    case SD_ERROR:
      return "Couldn't write to the SD card.";
    case DOWNLOAD_FAILED:
      return "The book didn't finish downloading.";
  }
  return "Unknown error";
}

bool AdeptClient::hasSession() { return g_session.loaded; }

AdeptClient::Error AdeptClient::loadSession() {
  g_session = AdeptSession{};

  std::string text;
  if (!Storage.readFileToString("ADEPT", SESSION_PATH, 32 * 1024, text)) return NOT_SET_UP;

  JsonDocument doc;
  if (deserializeJson(doc, text) != DeserializationError::Ok) return NOT_SET_UP;
  JsonObject act = doc["act"];
  if (act.isNull()) return NOT_SET_UP;

  g_session.userUuid = act["userUuid"] | "";
  g_session.deviceUuid = act["deviceUuid"] | "";
  g_session.signingKey = act["signingKey"] | "";
  g_session.signingCert = act["signingCert"] | "";
  g_session.licenseCert = act["licenseCertificate"] | "";
  g_session.activationUrl = act["activationURL"] | "";
  g_session.authUrl = act["authURL"] | "";
  g_session.deviceType = doc["device"]["deviceType"] | "standalone";

  if (g_session.userUuid.empty() || g_session.deviceUuid.empty() || g_session.signingKey.empty() ||
      g_session.activationUrl.empty()) {
    LOG_ERR("ADEPT", "session on card is incomplete");
    return NOT_SET_UP;
  }
  g_session.loaded = true;
  LOG_INF("ADEPT", "session loaded for %s", g_session.userUuid.c_str());
  return OK;
}

namespace {

// The shared body of fulfil() and refreshRights(): everything up to and
// including writing the .rights sidecar. `downloadUrlOut` receives the book URL
// so the caller can decide whether to fetch it.
AdeptClient::Error runFulfilment(const char* acsmPath, XmlNode& reply, std::string& downloadUrlOut,
                                 std::string& rightsXmlOut, char* titleOut, size_t titleLen,
                                 AdeptClient::ProgressFn progress, void* ctx) {
  if (!g_session.loaded) return AdeptClient::NOT_SET_UP;

  std::string acsmText;
  if (!Storage.readFileToString("ADEPT", acsmPath, 64 * 1024, acsmText)) return AdeptClient::SD_ERROR;

  XmlNode acsm;
  if (!parseXml(acsmText, acsm) || !named(acsm, "fulfillmentToken")) return AdeptClient::BAD_TOKEN;

  XmlNode* opUrlNode = findDeep(acsm, "operatorURL");
  if (!opUrlNode) return AdeptClient::BAD_TOKEN;
  std::string opUrl = trim(textOf(*opUrlNode));
  while (!opUrl.empty() && opUrl.back() == '/') opUrl.pop_back();
  const std::string fulfilUrl = opUrl + "/Fulfill";

  // Build the request around a copy of the token.
  XmlNode req;
  req.name = "adept:fulfill";
  req.attrs.emplace_back("xmlns:adept", ADEPT_NS);
  req.kids.push_back(textElement("adept:user", g_session.userUuid));
  req.kids.push_back(textElement("adept:device", g_session.deviceUuid));
  req.kids.push_back(textElement("adept:deviceType", g_session.deviceType));
  req.kids.push_back(acsm);

  // The server verifies the token's hmac against the UNSIGNED token, so lift it
  // out, sign, then put it back exactly where it was.
  XmlNode* tokenCopy = findDeep(req, "fulfillmentToken");
  if (!tokenCopy) return AdeptClient::BAD_TOKEN;
  std::string hmacText;
  bool foundHmac = false;
  for (auto it = tokenCopy->kids.begin(); it != tokenCopy->kids.end(); ++it) {
    if (named(*it, "hmac")) {
      hmacText = trim(textOf(*it));
      tokenCopy->kids.erase(it);
      foundHmac = true;
      break;
    }
  }
  if (!foundHmac) {
    LOG_ERR("ADEPT", "token carried no hmac");
    return AdeptClient::BAD_TOKEN;
  }
  if (!signNode(req)) return AdeptClient::SIGN_FAILED;
  // findDeep again: signNode appended to req, which may have reallocated kids.
  tokenCopy = findDeep(req, "fulfillmentToken");
  if (!tokenCopy) return AdeptClient::BAD_TOKEN;
  tokenCopy->kids.push_back(textElement("hmac", hmacText));

  std::string err;
  if (progress) progress(ctx, "Authenticating…", 0, 0);
  AdeptClient::Error e = operatorAuth(fulfilUrl, err);
  if (e != AdeptClient::OK) return e;

  if (progress) progress(ctx, "Preparing loan…", 0, 0);
  e = sendAdept(fulfilUrl, req, reply, err);
  if (e == AdeptClient::DISTRIBUTOR_AUTH) {
    // Re-introduce and retry once: the operator can expire a session.
    g_session.authedOperators.clear();
    e = operatorAuth(fulfilUrl, err);
    if (e != AdeptClient::OK) return e;
    e = sendAdept(fulfilUrl, req, reply, err);
  }
  if (e != AdeptClient::OK) return e;

  XmlNode* item = findDeep(reply, "resourceItemInfo");
  if (!item) return AdeptClient::SERVER_ERROR;
  XmlNode* src = findChild(*item, "src");
  XmlNode* licenseToken = findChild(*item, "licenseToken");
  if (!src || !licenseToken) return AdeptClient::SERVER_ERROR;
  downloadUrlOut = trim(textOf(*src));

  XmlNode* licenseUrlNode = findDeep(*licenseToken, "licenseURL");
  const std::string licenseUrl = licenseUrlNode ? trim(textOf(*licenseUrlNode)) : std::string();

  XmlNode* titleNode = findDeep(reply, "title");
  sanitiseTitle(titleNode ? trim(textOf(*titleNode)) : std::string(), titleOut, titleLen);

  // The licence service certificate goes into the rights document so the reader
  // can validate the licence offline.
  std::string licenseCert;
  if (!licenseUrl.empty()) {
    XmlNode info;
    std::string infoUrl = opUrl + "/LicenseServiceInfo?licenseURL=";
    for (const char c : licenseUrl) {
      if (isalnum(static_cast<unsigned char>(c)) || strchr("-_.~", c))
        infoUrl += c;
      else {
        char esc[4];
        snprintf(esc, sizeof(esc), "%%%02X", static_cast<unsigned char>(c));
        infoUrl += esc;
      }
    }
    if (httpGetXml(infoUrl, info) == AdeptClient::OK) {
      if (XmlNode* cert = findDeep(info, "certificate")) licenseCert = trim(textOf(*cert));
    }
  }

  // rights.xml travels beside the book, so the .epub stays byte-identical to
  // what the distributor sent.
  XmlNode rights;
  rights.name = "adept:rights";
  rights.attrs.emplace_back("xmlns:adept", ADEPT_NS);
  XmlNode tokenForRights = *licenseToken;
  if (!tokenForRights.attr("xmlns")) tokenForRights.attrs.emplace_back("xmlns", ADEPT_NS);
  rights.kids.push_back(std::move(tokenForRights));
  XmlNode serviceInfo;
  serviceInfo.name = "adept:licenseServiceInfo";
  serviceInfo.kids.push_back(textElement("adept:licenseURL", licenseUrl));
  serviceInfo.kids.push_back(textElement("adept:certificate", licenseCert));
  rights.kids.push_back(std::move(serviceInfo));
  rightsXmlOut = serialize(rights);

  return AdeptClient::OK;
}

bool writeText(const char* path, const std::string& text) {
  HalFile f;
  if (!Storage.openFileForWrite("ADEPT", path, f)) return false;
  const size_t n = f.write(text.data(), text.size());
  f.flush();
  f.close();
  return n == text.size();
}

}  // namespace

AdeptClient::Error AdeptClient::fulfil(const char* acsmPath, const char* destDir, Result& out,
                                       const ProgressFn progress, void* ctx) {
  XmlNode reply;
  std::string downloadUrl;
  std::string rightsXml;
  const Error e = runFulfilment(acsmPath, reply, downloadUrl, rightsXml, out.title, sizeof(out.title), progress, ctx);
  if (e != OK) return e;

  Storage.ensureDirectoryExists(destDir);
  if (!uniqueDestination(destDir, out.title, out.destPath, sizeof(out.destPath))) return SD_ERROR;

  // Licence first: if the transfer dies afterwards, re-sending the loan reuses
  // this rather than repeating the whole exchange.
  char rightsPath[160];
  snprintf(rightsPath, sizeof(rightsPath), "%s.rights", out.destPath);
  if (!writeText(rightsPath, rightsXml)) return SD_ERROR;

  if (progress) progress(ctx, "Downloading…", 0, 0);

  HalFile file;
  if (!Storage.openFileForWrite("ADEPT", out.destPath, file)) return SD_ERROR;
  if (!acquireClient()) return NETWORK_ERROR;
  if (!g_client->begin(downloadUrl)) {
    file.close();
    Storage.remove(out.destPath);
    dropClient();
    return NETWORK_ERROR;
  }

  size_t written = 0;
  bool sdFull = false;
  bool aborted = false;
  const int status = g_client->GET(
      [&](const uint8_t* data, size_t len) {
        if (file.write(data, len) != len) {
          sdFull = true;
          return false;
        }
        written += len;
        if (progress &&
            !progress(ctx, "Downloading…", written, g_client->hasContentLength() ? g_client->getContentLength() : 0)) {
          aborted = true;
          return false;
        }
        return true;
      },
      [&]() { return aborted; });
  file.flush();
  file.close();

  if (status < 0) dropClient();
  const bool complete = status >= 200 && status < 300 && !sdFull && !aborted && g_client->responseComplete();
  if (!complete) {
    LOG_ERR("ADEPT", "download failed after %u bytes (HTTP %d%s)", static_cast<unsigned>(written), status,
            sdFull ? ", SD full" : "");
    Storage.remove(out.destPath);
    Storage.remove(rightsPath);
    return sdFull ? SD_ERROR : DOWNLOAD_FAILED;
  }

  out.bytes = written;
  LOG_INF("ADEPT", "fulfilled '%s' -> %s (%u bytes)", out.title, out.destPath, static_cast<unsigned>(written));
  return OK;
}

AdeptClient::Error AdeptClient::refreshRights(const char* acsmPath, const char* bookPath, const ProgressFn progress,
                                              void* ctx) {
  XmlNode reply;
  std::string downloadUrl;
  std::string rightsXml;
  char title[72] = {};
  const Error e = runFulfilment(acsmPath, reply, downloadUrl, rightsXml, title, sizeof(title), progress, ctx);
  if (e != OK) return e;

  char rightsPath[160];
  snprintf(rightsPath, sizeof(rightsPath), "%s.rights", bookPath);
  if (!writeText(rightsPath, rightsXml)) return SD_ERROR;
  LOG_INF("ADEPT", "licence refreshed for %s", bookPath);
  return OK;
}

void AdeptClient::closeConnection() { dropClient(); }
