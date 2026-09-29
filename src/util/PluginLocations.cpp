#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cstring>

#include "PluginLocations.h"

namespace PluginLocations {

bool isNativeFeature(const char* name) {
  if (!name) return false;
  for (const char* prefix : kNativeFeaturePrefixes) {
    if (strncasecmp(name, prefix, strlen(prefix)) == 0) return true;
  }
  return false;
}

std::vector<Entry> scanPlugins() {
  std::vector<Entry> plugins;
  plugins.reserve(8);
  std::vector<std::string> seen;
  seen.reserve(8);
  for (size_t r = 0; r < kRootCount; r++) {
    HalFile root = Storage.open(kRoots[r]);
    if (!root || !root.isDirectory()) continue;
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) continue;
      char name[128];
      if (entry.getName(name, sizeof(name)) == 0 || name[0] == '.') continue;
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      if (isNativeFeature(name)) {
        LOG_INF("PLG", "Ignoring plugin '%s': this firmware has that feature built in", name);
        seen.emplace_back(name);
        continue;
      }
      // Claimed even without markers: findPluginDir resolves this name here,
      // so a same-named folder in a later root must not be reported instead.
      seen.emplace_back(name);

      Entry e;
      e.name = name;
      e.dir = std::string(kRoots[r]) + "/" + name;
      e.hasPluginJs = Storage.exists((e.dir + "/plugin.js").c_str());
      e.hasDevice = Storage.exists((e.dir + "/device.json").c_str());
      e.hasManifest = Storage.exists((e.dir + "/manifest.json").c_str());
      if (e.hasPluginJs || e.hasDevice || e.hasManifest) plugins.push_back(std::move(e));
    }
  }
  return plugins;
}

std::string findPluginDir(const char* name) {
  if (isNativeFeature(name)) return {};  // never served, even if the folder exists
  for (size_t i = 0; i < kRootCount; i++) {
    std::string dir = std::string(kRoots[i]) + "/" + name;
    if (Storage.exists(dir.c_str())) return dir;
  }
  return {};
}

}  // namespace PluginLocations

#endif  // CROSSPOINT_SD_PLUGINS
