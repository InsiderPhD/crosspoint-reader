#pragma once
#include <string>

namespace UrlUtils {

/**
 * Check if URL uses HTTPS protocol
 */
bool isHttpsUrl(const std::string& url);

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

/**
 * True when an HTTP Host header ("name[:port]") names another site. On the
 * reader's hotspot an IP literal, a .local name or a single-label name can only
 * mean the reader; any other name is an OS captive-portal check (or a site the
 * user typed) that the hotspot's wildcard DNS answered with the reader's
 * address. An empty or null host is not foreign.
 */
bool isForeignHost(const char* hostHeader);

}  // namespace UrlUtils
