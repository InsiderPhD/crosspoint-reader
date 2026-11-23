#include "DownloadFailureLog.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <sdkconfig.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "util/TimeUtils.h"

namespace {

constexpr const char* kOldPath = "/.crosspoint/download_failure.log.old";

// Appends `text` (no newline handling of its own) to the log file. Opening in
// append mode per call keeps no handle across the transfer.
void appendRaw(const char* text) {
  Storage.ensureDirectoryExists("/.crosspoint");
  FsFile file = Storage.open(DownloadFailureLog::kPath, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("DLLOG", "Cannot open %s", DownloadFailureLog::kPath);
    return;
  }
  file.write(text, strlen(text));
  file.close();
}

void rotateIfLarge() {
  size_t size = 0;
  {
    FsFile file = Storage.open(DownloadFailureLog::kPath, O_RDONLY);
    if (!file) return;
    size = file.fileSize();
    file.close();
  }
  if (size < DownloadFailureLog::kMaxBytes) return;
  Storage.remove(kOldPath);
  Storage.rename(DownloadFailureLog::kPath, kOldPath);
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID_AVAIL";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SHIELD:
      return "NO_SHIELD";
    default:
      return "OTHER";
  }
}

}  // namespace

namespace DownloadFailureLog {

void line(const char* fmt, ...) {
  char buf[192];
  const int prefix = snprintf(buf, sizeof(buf), "[+%lums] ", static_cast<unsigned long>(millis()));
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf + prefix, sizeof(buf) - prefix - 1, fmt, args);
  va_end(args);
  strlcat(buf, "\n", sizeof(buf));
  appendRaw(buf);
  LOG_DBG("DLLOG", "%s", buf + prefix);
}

void section(const char* title) {
  rotateIfLarge();
  char when[32] = "clock not set";
  if (TimeUtils::isClockValid()) {
    strlcpy(when, TimeUtils::formatDateTime(TimeUtils::getCurrentValidTimestamp()).c_str(), sizeof(when));
  }
  line("==== %s | %s | fw %s | target %s ====", title, when, CROSSPOINT_VERSION, CONFIG_IDF_TARGET);
}

void environment(const char* label) {
  line("%s: heap free=%u largest=%u min-ever=%u; task stack free=%u", label, (unsigned)ESP.getFreeHeap(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT), (unsigned)ESP.getMinFreeHeap(),
       (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  const wl_status_t status = WiFi.status();
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  if (status == WL_CONNECTED) {
    line("%s: wifi %s ssid=\"%s\" rssi=%d ch=%d ps=%d ip=%s gw=%s dns=%s", label, wifiStatusName(status),
         WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.channel(), static_cast<int>(ps), WiFi.localIP().toString().c_str(),
         WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  } else {
    line("%s: wifi %s (status %d) ps=%d", label, wifiStatusName(status), static_cast<int>(status),
         static_cast<int>(ps));
  }
}

void url(const char* label, const char* fullUrl) {
  // Copy up to the '?' verbatim, then only parameter names: "a=&b=&c=".
  char buf[160];
  size_t out = 0;
  const char* p = fullUrl;
  for (; *p != '\0' && *p != '?' && out + 1 < sizeof(buf); ++p) buf[out++] = *p;
  if (*p == '?') {
    bool inValue = false;
    for (; *p != '\0' && out + 1 < sizeof(buf); ++p) {
      if (*p == '&' || *p == '?') {
        inValue = false;
        buf[out++] = *p;
      } else if (*p == '=') {
        inValue = true;
        buf[out++] = *p;
      } else if (!inValue) {
        buf[out++] = *p;
      }
    }
  }
  buf[out] = '\0';
  line("%s: %s (len %u%s)", label, buf, (unsigned)strlen(fullUrl), *p != '\0' ? ", redacted+truncated" : "");
}

namespace {

// One A-record query straight to `server` over UDP (bypassing this network's
// resolver), so a DNS filter shows up as a different answer. Returns false
// with `why` set when there is no usable answer.
bool directDnsA(const char* host, const IPAddress& server, IPAddress& out, const char*& why) {
  uint8_t pkt[300];
  size_t n = 0;
  const uint16_t id = static_cast<uint16_t>(esp_random());
  const uint8_t header[12] = {static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id),
                              0x01,
                              0x00,
                              0,
                              1,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0};  // recursion desired, one question
  memcpy(pkt, header, sizeof(header));
  n = sizeof(header);
  for (const char* label = host; *label;) {
    const char* dot = strchr(label, '.');
    const size_t len = dot ? static_cast<size_t>(dot - label) : strlen(label);
    if (len == 0 || len > 63 || n + len + 6 > sizeof(pkt)) {
      why = "bad name";
      return false;
    }
    pkt[n++] = static_cast<uint8_t>(len);
    memcpy(pkt + n, label, len);
    n += len;
    label += len + (dot ? 1 : 0);
    if (!dot) break;
  }
  pkt[n++] = 0;
  pkt[n++] = 0, pkt[n++] = 1;  // A
  pkt[n++] = 0, pkt[n++] = 1;  // IN
  WiFiUDP udp;
  if (!udp.begin(0)) {
    why = "no udp socket";
    return false;
  }
  udp.beginPacket(server, 53);
  udp.write(pkt, n);
  if (!udp.endPacket()) {
    udp.stop();
    why = "send failed";
    return false;
  }
  const unsigned long t0 = millis();
  int got = 0;
  while (millis() - t0 < 1500) {
    if (udp.parsePacket() > 0) {
      got = udp.read(pkt, sizeof(pkt));
      if (got >= 12 && pkt[0] == static_cast<uint8_t>(id >> 8) && pkt[1] == static_cast<uint8_t>(id)) break;
      got = 0;
    }
    delay(10);
  }
  udp.stop();
  if (got < 12) {
    why = "no reply (udp 53 blocked?)";
    return false;
  }
  const uint8_t rcode = pkt[3] & 0x0F;
  if (rcode != 0) {
    why = rcode == 3 ? "NXDOMAIN" : rcode == 5 ? "REFUSED" : "error rcode";
    return false;
  }
  const uint16_t answers = static_cast<uint16_t>(pkt[6] << 8 | pkt[7]);
  size_t p = 12;
  const auto skipName = [&]() {
    while (p < static_cast<size_t>(got)) {
      const uint8_t len = pkt[p];
      if ((len & 0xC0) == 0xC0) {
        p += 2;
        return;
      }
      p += 1 + len;
      if (len == 0) return;
    }
  };
  skipName();
  p += 4;  // question type + class
  for (uint16_t i = 0; i < answers && p + 10 <= static_cast<size_t>(got); ++i) {
    skipName();
    if (p + 10 > static_cast<size_t>(got)) break;
    const uint16_t type = static_cast<uint16_t>(pkt[p] << 8 | pkt[p + 1]);
    const uint16_t rdlen = static_cast<uint16_t>(pkt[p + 8] << 8 | pkt[p + 9]);
    p += 10;
    if (type == 1 && rdlen == 4 && p + 4 <= static_cast<size_t>(got)) {
      out = IPAddress(pkt[p], pkt[p + 1], pkt[p + 2], pkt[p + 3]);
      return true;
    }
    p += rdlen;  // CNAMEs come first: skip to the A record
  }
  why = "no A record";
  return false;
}

const char* sinkholeFlag(const IPAddress& ip) {
  if (ip == IPAddress(0, 0, 0, 0)) return " SINKHOLED(0.0.0.0)";
  if (ip[0] == 127) return " SINKHOLED(loopback)";
  if (ip[0] == 10 || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)) {
    return " PRIVATE(filter/block page?)";
  }
  return "";
}

bool tcpOpen(const IPAddress& ip, unsigned long& ms) {
  WiFiClient tcp;
  const unsigned long t0 = millis();
  const bool open = tcp.connect(ip, 443, 3000) == 1;
  ms = millis() - t0;
  tcp.stop();
  return open;
}

// What one host's probe found, for the verdict.
struct ProbeResult {
  bool sysDns = false, sinkholed = false, publicDns = false, tcpSys = false, tcpPublic = false, https = false;
  bool udpBlocked = false;
};
ProbeResult lastProbe;

bool probedThisBoot = false;

}  // namespace

void hostProbe(const char* host) {
  lastProbe = ProbeResult{};
  if (!host || !*host) return;
  if (WiFi.status() != WL_CONNECTED) {
    line("probe %s: skipped, wifi not connected", host);
    return;
  }
  esp_task_wdt_reset();
  ProbeResult& r = lastProbe;

  // 1. This network's resolver.
  const unsigned long t0 = millis();
  IPAddress sysIp;
  r.sysDns = WiFi.hostByName(host, sysIp) == 1;
  const unsigned long dnsMs = millis() - t0;
  const char* flag = r.sysDns ? sinkholeFlag(sysIp) : "";
  r.sinkholed = *flag != '\0';
  if (r.sysDns) {
    line("probe %s: dns(%s) %s%s in %lums", host, WiFi.dnsIP().toString().c_str(), sysIp.toString().c_str(), flag,
         dnsMs);
  } else {
    line("probe %s: dns(%s) FAILED after %lums", host, WiFi.dnsIP().toString().c_str(), dnsMs);
  }

  // 2. Public resolvers asked directly.
  IPAddress pubIp;
  for (const IPAddress server : {IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8)}) {
    IPAddress ip;
    const char* why = "";
    const unsigned long t1 = millis();
    if (directDnsA(host, server, ip, why)) {
      line("probe %s: dns(%s direct) %s%s in %lums", host, server.toString().c_str(), ip.toString().c_str(),
           sinkholeFlag(ip), millis() - t1);
      if (!r.publicDns) pubIp = ip;
      r.publicDns = true;
    } else {
      line("probe %s: dns(%s direct) FAILED: %s", host, server.toString().c_str(), why);
      if (strstr(why, "udp")) r.udpBlocked = true;
    }
  }
  esp_task_wdt_reset();

  // 3. TCP 443 at each address.
  unsigned long ms = 0;
  if (r.sysDns && !r.sinkholed) {
    r.tcpSys = tcpOpen(sysIp, ms);
    line("probe %s: tcp %s:443 %s in %lums", host, sysIp.toString().c_str(), r.tcpSys ? "open" : "FAILED", ms);
  }
  if (r.publicDns && (!r.sysDns || pubIp != sysIp)) {
    r.tcpPublic = tcpOpen(pubIp, ms);
    line("probe %s: tcp %s:443 (public dns) %s in %lums", host, pubIp.toString().c_str(),
         r.tcpPublic ? "open" : "FAILED", ms);
  } else {
    r.tcpPublic = r.tcpSys;
  }
  esp_task_wdt_reset();

  // 4. HTTPS HEAD through this network's resolver, as the download does.
  if (r.sysDns && !r.sinkholed && r.tcpSys) {
    freeink::SecureHttpClient http;
    char url[96];
    snprintf(url, sizeof(url), "https://%s/", host);
    if (http.begin(url)) {
      http.setInsecure();
      http.setFollowRedirects(0);
      http.setTimeout(6000);
      http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
      const unsigned long t2 = millis();
      const int code = http.sendRequest("HEAD", nullptr, 0);
      const unsigned long httpsMs = millis() - t2;
      if (code > 0) {
        r.https = true;
        const std::string server = http.getHeader("server");
        const std::string location = http.getHeader("location");
        line("probe %s: https HEAD %d in %lums, tls %s, server=\"%.40s\" location=\"%.60s\"", host, code, httpsMs,
             http.hopCount() ? http.hops()[0].tls : "?", server.c_str(), location.c_str());
      } else {
        const auto& d = http.lastFailure();
        line("probe %s: https FAILED at %s in %lums: connect=%s tls=%d \"%s\" alert=%d", host,
             d.stage[0] ? d.stage : "?", httpsMs, d.connectStage[0] ? d.connectStage : "-", d.tlsError,
             d.tlsErrorString, d.tlsAlert);
      }
      http.end();
    }
  }
  if (!r.sysDns || r.sinkholed || !r.tcpSys || !r.https) {
    LOG_ERR("DLLOG", "probe %s: dns=%d%s public=%d tcp=%d/%d https=%d", host, r.sysDns ? 1 : 0, flag,
            r.publicDns ? 1 : 0, r.tcpSys ? 1 : 0, r.tcpPublic ? 1 : 0, r.https ? 1 : 0);
  }
}

void githubProbe(const char* failingHost) {
  if (probedThisBoot) {
    line("probe: already run this boot (see the report above)");
    return;
  }
  probedThisBoot = true;
  static constexpr const char* kHosts[] = {"api.github.com", "github.com", "raw.githubusercontent.com",
                                           "release-assets.githubusercontent.com", "objects.githubusercontent.com"};
  bool failingListed = !failingHost || !*failingHost;
  for (const char* h : kHosts) failingListed = failingListed || strcmp(h, failingHost) == 0;
  if (!failingListed) hostProbe(failingHost);

  // The verdict looks at the hosts the downloads actually fetch from.
  int dnsFiltered = 0, ipBlocked = 0, tlsBlocked = 0, fine = 0, total = 0;
  bool udpBlocked = false;
  for (const char* h : kHosts) {
    hostProbe(h);
    const ProbeResult& r = lastProbe;
    udpBlocked = udpBlocked || r.udpBlocked;
    if (strcmp(h, "api.github.com") == 0) continue;  // the update check, not a download host
    ++total;
    if ((!r.sysDns || r.sinkholed) && r.publicDns) {
      ++dnsFiltered;
    } else if (r.sysDns && !r.sinkholed && !r.tcpSys && !r.tcpPublic) {
      ++ipBlocked;
    } else if (r.tcpSys && !r.https) {
      ++tlsBlocked;
    } else if (r.https) {
      ++fine;
    }
  }
  // Short lines: the log formats into a 192-byte buffer.
  const char* verdict;
  const char* fix = "";
  if (fine == total) {
    verdict = "NOT A NETWORK BLOCK: every GitHub host answers HTTPS from here";
    fix = "see the transport/TLS and hop lines above";
  } else if (dnsFiltered > 0) {
    verdict = "DNS FILTER: this network's resolver hides hosts 1.1.1.1/8.8.8.8 resolve";
    fix = "router DNS -> 1.1.1.1 or 8.8.8.8; or ISP filter / Pi-hole / NextDNS / parental controls";
  } else if (ipBlocked > 0) {
    verdict = "IP BLOCK: hosts resolve but TCP 443 is refused";
    fix = "firewall or ISP block; try another network or a phone hotspot";
  } else if (tlsBlocked > 0) {
    verdict = "TLS BLOCK: TCP opens but HTTPS fails";
    fix = "filtering proxy intercepting TLS, or a TLS bug here (see the https lines)";
  } else {
    verdict = "MIXED: see the probe lines";
  }
  line("verdict: %s (download hosts ok %d/%d%s)", verdict, fine, total, udpBlocked ? ", direct DNS blocked" : "");
  if (*fix) line("fix: %s", fix);
  LOG_ERR("DLLOG", "verdict: %s", verdict);
}

void fileHead(const char* label, const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("DLLOG", path, file)) {
    line("%s: %s cannot be opened", label, path);
    return;
  }
  const size_t size = file.fileSize();
  uint8_t head[64];
  const int n = file.read(head, sizeof(head));
  file.close();
  char text[sizeof(head) + 1];
  bool printable = n > 0;
  for (int i = 0; i < n; ++i) {
    const uint8_t c = head[i];
    printable = printable && (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F));
    text[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
  }
  text[n > 0 ? n : 0] = '\0';
  if (printable) {
    line("%s: %s (%u bytes) starts \"%s\"", label, path, (unsigned)size, text);
  } else {
    char hex[3 * 24 + 1] = {};
    for (int i = 0; i < n && i < 24; ++i) snprintf(hex + i * 3, 4, "%02x ", head[i]);
    line("%s: %s (%u bytes) starts %s", label, path, (unsigned)size, hex);
  }
}

}  // namespace DownloadFailureLog
