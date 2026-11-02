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
 * A server-relative path from a response ("/x", not "//host/x") gets the
 * origin (scheme, host, and port) of requestUrl, the request that returned it.
 * Returns true when path was changed. Other values (full URLs, "//host/x",
 * "files/12") are left alone.
 */
bool resolveServerPath(std::string& path, const std::string& requestUrl);
 * Resolve a link found in a document fetched from baseUrl, the way a browser
 * does (RFC 3986 section 5): an absolute URL as is, "//host/x" with the base's
 * scheme, "/x" against the base's host, "?q" against the base's path, and a
 * relative path against the base's directory with "." and ".." segments
 * removed. Unlike buildUrl(), "next.xml" on ".../opds/root.xml" gives
 * ".../opds/next.xml", not ".../opds/root.xml/next.xml".
 */
std::string resolveReference(const std::string& baseUrl, const std::string& reference);

/**
 * "scheme://host[:port]" of a URL, lowercased, without user info and without
 * the scheme's default port, so two spellings of one site compare equal.
 * Empty for a URL without "://".
 */
std::string origin(const std::string& url);

/**
 * True when an HTTP Host header ("name[:port]") names another site. On the
 * reader's hotspot an IP literal, a .local name or a single-label name can only
 * mean the reader; any other name is an OS captive-portal check (or a site the
 * user typed) that the hotspot's wildcard DNS answered with the reader's
 * address. An empty or null host is not foreign.
 */
bool isForeignHost(const char* hostHeader);

}  // namespace UrlUtils
