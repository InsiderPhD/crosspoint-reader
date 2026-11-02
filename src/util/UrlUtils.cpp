#include "UrlUtils.h"

#include <strings.h>

#include <cctype>
#include <cstring>

namespace UrlUtils {

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}

std::string extractHost(const std::string& url) {
  const size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    // No protocol, find first slash
    const size_t firstSlash = url.find('/');
    return firstSlash == std::string::npos ? url : url.substr(0, firstSlash);
  }
  // Find the first slash after the protocol
  const size_t hostStart = protocolEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  // If path is already an absolute URL (has protocol), use it directly
  if (path.find("://") != std::string::npos) {
    return path;
  }
  const std::string urlWithProtocol = ensureProtocol(serverUrl);
  if (path.empty()) {
    return urlWithProtocol;
  }
  if (path[0] == '/') {
    // Absolute path - use just the host
    return extractHost(urlWithProtocol) + path;
  }
  // Relative path - strip query string from base before appending
  std::string base = urlWithProtocol;
  const size_t queryPos = base.find('?');
  if (queryPos != std::string::npos) {
    base.resize(queryPos);
  }
  if (base.back() == '/') {
    return base + path;
  }
  return base + "/" + path;
}

bool isForeignHost(const char* hostHeader) {
  if (hostHeader == nullptr) return false;  // a String whose allocation failed
  const size_t len = strcspn(hostHeader, ":");
  if (len == 0 || hostHeader[0] == '[') return false;  // no host, or an IPv6 literal
  constexpr char LOCAL_SUFFIX[] = ".local";
  constexpr size_t SUFFIX_LEN = sizeof(LOCAL_SUFFIX) - 1;
  if (len >= SUFFIX_LEN && strncasecmp(hostHeader + len - SUFFIX_LEN, LOCAL_SUFFIX, SUFFIX_LEN) == 0) return false;
  bool hasDot = false;
  bool allDigits = true;
  for (size_t i = 0; i < len; i++) {
    const char c = hostHeader[i];
    if (c == '.') {
      hasDot = true;
    } else if (!isdigit(static_cast<unsigned char>(c))) {
      allDigits = false;
    }
  }
  return hasDot && !allDigits;
}

}  // namespace UrlUtils
