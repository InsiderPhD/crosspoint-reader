#include "SleepThemePluginVars.h"

#include <DevicePolicy.h>

#include <cstdio>
#include <cstring>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <memory>
#include <string>

#include "util/PluginHttp.h"
#include "util/PluginLocations.h"
#endif

namespace SleepThemePluginVars {

namespace {

constexpr char PREFIX[] = "plugin.";
constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;

bool isSegmentChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Length of the run of segment characters at the start of s (at most len).
size_t segmentLen(const char* s, const size_t len) {
  size_t i = 0;
  while (i < len && isSegmentChar(s[i])) i++;
  return i;
}

}  // namespace

bool isPluginVar(const char* name, const size_t nameLen) {
  if (nameLen <= PREFIX_LEN || strncmp(name, PREFIX, PREFIX_LEN) != 0) return false;
  const char* p = name + PREFIX_LEN;
  const size_t rest = nameLen - PREFIX_LEN;
  const size_t pluginLen = segmentLen(p, rest);
  if (pluginLen == 0 || pluginLen >= rest || p[pluginLen] != '.') return false;
  const size_t keyLen = rest - pluginLen - 1;
  return keyLen > 0 && segmentLen(p + pluginLen + 1, keyLen) == keyLen;
}

#if CROSSPOINT_SD_PLUGINS

namespace {

// Longest plugin or key segment read. Folder names are far shorter in practice;
// this keeps the stack buffers below fixed and small.
constexpr size_t MAX_SEGMENT = 63;
// Plugins whose values one frame keeps. A theme drawing from more than this
// many plugins still works: the rest are read per lookup.
constexpr uint8_t MAX_CACHED = 4;

struct CachedPlugin {
  char segment[MAX_SEGMENT + 1] = {};
  pluginhttp::Headers values;
};
// Heap, between beginFrame() and endFrame() only, like SleepThemeVars' own
// frame cache: the screen is drawn once per sleep, so holding this in static
// DRAM all the time would waste it. Only built on the PSRAM boards.
struct FrameCache {
  CachedPlugin plugins[MAX_CACHED];
  uint8_t count = 0;
};
std::unique_ptr<FrameCache> cache;

// A folder name as a theme writes it: '-' becomes '_'. False when the folder
// name holds anything else a theme name cannot (then no value can reach it).
bool toSegment(const std::string& folder, char (&out)[MAX_SEGMENT + 1]) {
  if (folder.empty() || folder.size() > MAX_SEGMENT) return false;
  for (size_t i = 0; i < folder.size(); i++) {
    const char c = folder[i];
    if (c == '-') {
      out[i] = '_';
    } else if (isSegmentChar(c)) {
      out[i] = c;
    } else {
      return false;
    }
  }
  out[folder.size()] = '\0';
  return true;
}

// The folder of the plugin a theme segment names: the folder of that exact
// name, else the plugin whose name maps to it with '-' written as '_'. "" when
// there is none, or plugins are off.
std::string pluginDir(const char* segment) {
  std::string dir = PluginLocations::findPluginDir(segment);
  if (!dir.empty() || strchr(segment, '_') == nullptr) return dir;
  char mapped[MAX_SEGMENT + 1];
  for (const auto& plugin : PluginLocations::scanPlugins()) {
    if (toSegment(plugin.name, mapped) && strcmp(mapped, segment) == 0) return plugin.dir;
  }
  return "";
}

void loadValues(const std::string& dir, pluginhttp::Headers& out) {
  out.clear();
  if (dir.empty()) return;
  const std::string path = dir + "/" + FILE_NAME;
  // Most plugins publish nothing; don't let readFileToString log each of them.
  if (!Storage.exists(path.c_str())) return;
  pluginhttp::loadConfigFile(path, out);
}

// The plugin's values from the frame cache, loading them on first use. Null
// when no frame is open or the cache is full.
const pluginhttp::Headers* cachedValues(const char* segment) {
  if (!cache) return nullptr;
  for (uint8_t i = 0; i < cache->count; i++) {
    if (strcmp(cache->plugins[i].segment, segment) == 0) return &cache->plugins[i].values;
  }
  if (cache->count >= MAX_CACHED) return nullptr;
  CachedPlugin& slot = cache->plugins[cache->count++];
  snprintf(slot.segment, sizeof(slot.segment), "%s", segment);
  loadValues(pluginDir(segment), slot.values);
  return &slot.values;
}

}  // namespace

void lookup(const char* name, const size_t nameLen, char* out, const size_t outSize) {
  if (outSize == 0) return;
  out[0] = '\0';
  if (!isPluginVar(name, nameLen)) return;

  const char* p = name + PREFIX_LEN;
  const size_t pluginLen = segmentLen(p, nameLen - PREFIX_LEN);
  if (pluginLen > MAX_SEGMENT) return;
  char segment[MAX_SEGMENT + 1];
  memcpy(segment, p, pluginLen);
  segment[pluginLen] = '\0';
  const char* key = p + pluginLen + 1;
  const size_t keyLen = nameLen - PREFIX_LEN - pluginLen - 1;

  pluginhttp::Headers uncached;
  const pluginhttp::Headers* values = cachedValues(segment);
  if (values == nullptr) {
    loadValues(pluginDir(segment), uncached);
    values = &uncached;
  }
  for (const auto& kv : *values) {
    if (kv.first.size() == keyLen && memcmp(kv.first.data(), key, keyLen) == 0) {
      snprintf(out, outSize, "%s", kv.second.c_str());
      return;
    }
  }
}

void beginFrame() {
  cache = makeUniqueNoThrow<FrameCache>();
  if (!cache) LOG_ERR("STH", "malloc failed: %u bytes plugin value cache", static_cast<unsigned>(sizeof(FrameCache)));
}

void endFrame() { cache.reset(); }

void forEach(void (*fn)(void* ctx, const char* name, const char* value), void* ctx) {
  pluginhttp::Headers values;
  char segment[MAX_SEGMENT + 1];
  char varName[PREFIX_LEN + 2 * MAX_SEGMENT + 2];
  for (const auto& plugin : PluginLocations::scanPlugins()) {
    if (!toSegment(plugin.name, segment)) continue;
    loadValues(plugin.dir, values);
    for (const auto& kv : values) {
      const size_t keyLen = kv.first.size();
      // Keys a theme cannot name are left out rather than listed unusable.
      if (keyLen == 0 || keyLen > MAX_SEGMENT || segmentLen(kv.first.c_str(), keyLen) != keyLen) continue;
      snprintf(varName, sizeof(varName), "%s%s.%s", PREFIX, segment, kv.first.c_str());
      fn(ctx, varName, kv.second.c_str());
    }
  }
}

#else  // !CROSSPOINT_SD_PLUGINS: plugin values exist in themes but always read empty.

void lookup(const char*, size_t, char* out, const size_t outSize) {
  if (outSize > 0) out[0] = '\0';
}
void beginFrame() {}
void endFrame() {}
void forEach(void (*)(void*, const char*, const char*), void*) {}

#endif  // CROSSPOINT_SD_PLUGINS

}  // namespace SleepThemePluginVars
