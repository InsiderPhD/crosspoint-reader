#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace freeink {
class SecureHttpClient;
}

// Shared primitives of the SD-plugin declarative HTTP vocabulary, split out of
// PluginCatalogActivity so non-UI consumers (the plugin event drain) can run
// manifest-declared requests without the catalog screen. Substitution variable
// sets stay with the callers — each context ({page}/{query} vs {event.*})
// builds its own — while template mechanics, token/config storage, and the
// bounded transport live here.
namespace pluginhttp {

using Headers = std::vector<std::pair<std::string, std::string>>;

// --- JSON / template helpers ---

// Splits a dotted path ("authors.0.name") into segments.
void splitPath(const std::string& dotted, std::vector<std::string>& out);

// True when a dotted-path segment addresses an array index.
bool segIsIndex(const std::string& seg);

// Resolves a dotted path against a parsed document.
JsonVariantConst resolvePath(JsonVariantConst node, const std::string& dotted);

// String or integral variant as text; "" for anything else.
std::string variantToString(JsonVariantConst v);

// Replaces every occurrence of `key` in `s` with `value`.
void substituteAll(std::string& s, const char* key, const std::string& value);

// Reads a manifest "headers" object into name/value pairs.
void readHeaders(JsonVariantConst node, Headers& out);

// One declarative HTTP request as manifests express it. Shared by the catalog
// auth/poll groups and the event handlers so the {url, method, body, headers}
// shape is parsed in exactly one place.
struct RequestSpec {
  std::string url, method, body;
  Headers headers;
};

// Fills `out` from a manifest request node ({url, method, body, headers}).
void readRequest(JsonVariantConst node, const char* defaultMethod, RequestSpec& out);

// Percent-encodes a query value ('/' included).
std::string urlEncodeQuery(const std::string& s);

// Percent-encodes the bytes a request line cannot carry: spaces, control
// characters and non-ASCII (UTF-8) bytes. Every other printable ASCII byte is
// kept ('&', '?', '#', '/', '+', '=', '%'), so a URL that is already valid is
// unchanged and a raw {title} in a path still forms a valid request. Applied to
// every plugin request URL (upstream CrossPoint #4196).
std::string escapeUrl(const std::string& url);

// --- token / config files ---

// A manifest file path: relative paths live in the plugin's own folder
// (`pluginDir`), absolute paths are used as they are (upstream CrossPoint 2.0).
std::string inPluginDir(const std::string& pluginDir, const char* path);

// Reads the token at `path` (dotted) from the JSON file at `file`.
// An empty `file` means a token-less plugin: returns true with `out` empty.
// A credential store (wifi / koreader / settings ...) is refused, for reading
// and writing, so a manifest cannot point the firmware at one.
bool loadTokenFromFile(const std::string& file, const std::string& path, std::string& out);

// Writes `value` at `path` into `file`, building the nesting the read path
// expects (numeric segments unsupported). Removes the file on a short write so
// a truncated token is never persisted.
bool saveTokenToFile(const std::string& file, const std::string& path, const std::string& value);

// Loads a flat JSON config file into {cfg.KEY} pairs. Missing file = empty.
void loadConfigFile(const std::string& file, Headers& out);

// --- bounded transport ---
// `session` is an optional caller-owned TLS session reused across requests
// (repeated handshakes permanently fragment the heap); pass nullptr to use a
// per-call stack client.

// Request with the body collected in DRAM, capped at `maxResponse` with a
// nothrow-probed growth (an oversized response fails the request instead of
// aborting on low heap). Error statuses still return their body (OAuth
// polling carries state in 4xx bodies). Returns HTTP status, -1 on transport
// failure / truncation / cap.
int request(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
            const std::string& body, const Headers& headers, std::string& out, size_t maxResponse,
            uint32_t timeoutMs = 30000);

// Same request, body streamed to a file on SD instead of DRAM.
int requestToFile(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                  const std::string& body, const Headers& headers, const char* destPath, size_t maxResponse,
                  uint32_t timeoutMs = 30000);

// Password-grant token mint: runs the (already-substituted) auth request and
// extracts the token at `tokenPath` from a 2xx JSON response.
bool mintPasswordToken(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                       const std::string& body, const Headers& headers, const std::string& tokenPath,
                       std::string& outToken, uint32_t timeoutMs = 30000);

}  // namespace pluginhttp

#endif  // CROSSPOINT_SD_PLUGINS
