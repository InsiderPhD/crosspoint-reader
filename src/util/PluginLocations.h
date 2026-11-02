#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <cstdint>
#include <string>
#include <vector>

// Plugin folders may live under any of these SD roots; earlier roots win on
// name collisions. /.crosspoint/plugins is the historical home; /plugins and
// /.plugins are friendlier for users copying folders onto the card from a
// computer.
namespace PluginLocations {
inline constexpr const char* kRoots[] = {"/.crosspoint/plugins", "/plugins", "/.plugins"};
inline constexpr size_t kRootCount = sizeof(kRoots) / sizeof(kRoots[0]);

// Exactly MAJOR.MINOR.PATCH: three digit-only components, nothing else (no
// sign, spaces, suffix or fourth part), each fitting in 32 bits.
inline bool parseVersion(const std::string& v, uint32_t (&out)[3]) {
  size_t pos = 0;
  for (int k = 0; k < 3; k++) {
    if (k > 0 && (pos >= v.size() || v[pos++] != '.')) return false;
    if (pos >= v.size() || v[pos] < '0' || v[pos] > '9') return false;
    uint64_t n = 0;
    for (; pos < v.size() && v[pos] >= '0' && v[pos] <= '9'; pos++) {
      n = n * 10 + static_cast<uint64_t>(v[pos] - '0');
      if (n > UINT32_MAX) return false;
    }
    out[k] = static_cast<uint32_t>(n);
  }
  return pos == v.size();
}

// Plugin versions are MAJOR.MINOR.PATCH (the catalog contract). True when
// `catalog` is newer than `installed`, so an older catalog never offers a
// downgrade. An installed copy without a valid version is offered the
// update; a malformed catalog version never is.
inline bool isNewerVersion(const std::string& catalog, const std::string& installed) {
  uint32_t c[3], i[3];
  if (!parseVersion(catalog, c)) return false;
  if (!parseVersion(installed, i)) return true;
  for (int k = 0; k < 3; k++) {
    if (c[k] != i[k]) return c[k] > i[k];
  }
  return false;
}

// One SD plugin folder, classified by the marker files it carries.
struct Entry {
  std::string name;          // folder name
  std::string dir;           // "<root>/<name>"
  bool hasPluginJs = false;  // browser-side plugin (plugin.js)
  bool hasDevice = false;    // on-device catalog manifest (device.json)
  bool hasManifest = false;  // web UI card metadata (manifest.json)
};

// Plugins whose service this firmware already implements natively (BookFusion
// under File Transfer / Settings, dictionaries under Settings → Dictionary).
// A folder whose name starts with one of these (case-insensitive) is ignored
// by discovery, never served, and cannot be installed through /api/plugin-fs:
// two implementations of the same account/sync would fight over tokens and
// progress. Names follow the sd-plugins repository folders.
inline constexpr const char* kNativeFeaturePrefixes[] = {"bookfusion", "dictionar"};

// True when `name` is a plugin folder the native feature set supersedes.
bool isNativeFeature(const char* name);

// Scans every root. The earliest root containing a folder name claims it —
// matching findPluginDir, which serves that folder's files — and folders with
// none of the marker files are omitted. This is the single definition of
// "what is a plugin"; callers filter by the markers they need.
std::vector<Entry> scanPlugins();

// Directory of the named plugin ("<root>/<name>"), or "" when absent.
std::string findPluginDir(const char* name);
}  // namespace PluginLocations

#endif  // CROSSPOINT_SD_PLUGINS
