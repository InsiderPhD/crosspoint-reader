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

bool resolveServerPath(std::string& path, const std::string& requestUrl) {
  if (path.empty() || path[0] != '/' || (path.size() > 1 && path[1] == '/')) return false;
  const size_t scheme = requestUrl.find("://");
  if (scheme == std::string::npos) return false;
  const size_t hostEnd = requestUrl.find_first_of("/?#", scheme + 3);
  path.insert(0, requestUrl, 0, hostEnd == std::string::npos ? requestUrl.size() : hostEnd);
  return true;
namespace {
// RFC 3986 5.2.4, on a path without query or fragment.
std::string removeDotSegments(const std::string& path) {
  std::string out;
  out.reserve(path.size());
  size_t pos = 0;
  while (pos < path.size()) {
    size_t end = path.find('/', pos + 1);
    if (end == std::string::npos) end = path.size();
    // Segment including its leading '/', e.g. "/a" or "/..".
    const std::string segment = path.substr(pos, end - pos);
    if (segment == "/." || segment == ".") {
      if (end == path.size()) out += '/';
    } else if (segment == "/.." || segment == "..") {
      const size_t cut = out.rfind('/');
      out.resize(cut == std::string::npos ? 0 : cut);
      if (end == path.size()) out += '/';
    } else {
      out += segment;
    }
    pos = end;
  }
  return out.empty() ? std::string("/") : out;
}
}  // namespace

std::string resolveReference(const std::string& baseUrl, const std::string& reference) {
  if (reference.find("://") != std::string::npos) return reference;
  const std::string base = ensureProtocol(baseUrl);
  const std::string baseNoFragment = base.substr(0, base.find('#'));
  if (reference.empty()) return baseNoFragment;
  if (reference.rfind("//", 0) == 0) return base.substr(0, base.find("://") + 1) + reference;
  if (reference[0] == '#') return baseNoFragment + reference;
  const std::string basePath = baseNoFragment.substr(0, baseNoFragment.find('?'));
  if (reference[0] == '?') return basePath + reference;

  const std::string host = extractHost(basePath);
  // Split the reference's path from its query/fragment: only the path is
  // dot-normalised.
  const size_t tailPos = reference.find_first_of("?#");
  const std::string refPath = reference.substr(0, tailPos);
  const std::string refTail = tailPos == std::string::npos ? std::string() : reference.substr(tailPos);
  std::string path;
  if (reference[0] == '/') {
    path = refPath;
  } else {
    const std::string dirPath = basePath.size() > host.size() ? basePath.substr(host.size()) : std::string("/");
    path = dirPath.substr(0, dirPath.rfind('/') + 1) + refPath;
  }
  return host + removeDotSegments(path) + refTail;
}

std::string origin(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return "";
  std::string scheme = url.substr(0, schemeEnd);
  const size_t hostStart = schemeEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", hostStart);
  std::string authority =
      url.substr(hostStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - hostStart);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority.erase(0, at + 1);
  for (auto& c : scheme) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  for (auto& c : authority) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  const size_t colon = authority.rfind(':');
  if (colon != std::string::npos && authority.find(']', colon) == std::string::npos) {
    const std::string port = authority.substr(colon + 1);
    if ((scheme == "http" && port == "80") || (scheme == "https" && port == "443") || port.empty()) {
      authority.resize(colon);
    }
  }
  return scheme + "://" + authority;
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
