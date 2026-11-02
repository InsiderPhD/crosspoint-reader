#include "SleepImage.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string_view>

#include "CrossPointState.h"

namespace SleepImage {

namespace {
constexpr char ROOT_SLEEP_IMAGE[] = "/sleep.bmp";
constexpr char DEFAULT_SLEEP_DIR_HIDDEN[] = "/.sleep";
constexpr char DEFAULT_SLEEP_DIR[] = "/sleep";

// Non-zero 16-bit FNV-1a fold of a folder path: tags which folder the
// recently-shown history in APP_STATE was counted in.
uint16_t sleepFolderHash(const std::string_view path) {
  uint32_t hash = 2166136261u;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  const auto folded = static_cast<uint16_t>((hash >> 16) ^ (hash & 0xFFFFu));
  return folded == 0 ? 1 : folded;
}

bool openSleepDir(const char* path, FsFile& dir) {
  dir = Storage.open(path);
  if (dir && dir.isDirectory()) {
    return true;
  }
  if (dir) dir.close();
  return false;
}

// Picks one .bmp from the open directory `dir` (whose
// path is `dirPath`) and returns its full path in `outPath`. Records the pick
// in APP_STATE's history.
//
// One pass, two candidates, no list. Holding every filename in a vector and
// opening every BMP to parse its header made sleep entry scale with the
// folder (two card seeks per file, plus one heap block per name). Instead:
//  - nonRecentName: a uniform reservoir sample over the images that are not
//    in the recently-shown history;
//  - oldestRecentName: of the images that are in it, the one shown longest
//    ago. Used only when every image is recent (a folder smaller than the
//    history), where it gives a strict rotation rather than a repeat.
// The caller validates the pick's header once, after selection.
bool pickSleepImage(FsFile& dir, const std::string& dirPath, std::string& outPath) {
  // The history is a list of positions in ONE folder's listing. Against any
  // other folder its indices would exclude arbitrary images, so start over.
  const uint16_t folderHash = sleepFolderHash(dirPath);
  if (APP_STATE.recentSleepFolderHash != 0 && APP_STATE.recentSleepFolderHash != folderHash) {
    APP_STATE.clearRecentSleepHistory();
  }
  APP_STATE.recentSleepFolderHash = folderHash;

  std::string nonRecentName;
  std::string oldestRecentName;
  uint16_t candidateCount = 0;
  uint16_t nonRecentCount = 0;
  uint16_t nonRecentIndex = 0;
  uint16_t oldestRecentIndex = 0;
  uint8_t oldestRecentAge = 0;
  bool haveOldestRecent = false;
  char name[500];
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) {
      dirFile.close();
      continue;
    }
    dirFile.getName(name, sizeof(name));
    dirFile.close();
    const std::string_view filename(name);
    if (filename.empty() || filename.front() == '.') {
      continue;
    }

    if (!FsHelpers::hasBmpExtension(filename)) {
      LOG_DBG("SLP", "Skipping non-image file name: %s", name);
      continue;
    }

    if (candidateCount == UINT16_MAX) {
      break;
    }
    const uint16_t candidateIndex = candidateCount++;

    const uint8_t age = APP_STATE.recentSleepAge(candidateIndex);
    if (age == CrossPointState::SLEEP_NOT_RECENT) {
      nonRecentCount++;
      // Reservoir sampling: the k-th eligible image replaces the pick with
      // probability 1/k, which leaves every eligible image equally likely.
      if (random(nonRecentCount) == 0) {
        nonRecentName.assign(filename.data(), filename.size());
        nonRecentIndex = candidateIndex;
      }
    } else if (!haveOldestRecent || age > oldestRecentAge) {
      oldestRecentName.assign(filename.data(), filename.size());
      oldestRecentIndex = candidateIndex;
      oldestRecentAge = age;
      haveOldestRecent = true;
    }
  }
  if (candidateCount == 0) {
    return false;
  }

  const bool useNonRecent = nonRecentCount > 0;
  outPath = dirPath;
  if (outPath.empty() || outPath.back() != '/') outPath += '/';
  outPath += useNonRecent ? nonRecentName : oldestRecentName;
  APP_STATE.pushRecentSleep(useNonRecent ? nonRecentIndex : oldestRecentIndex);
  APP_STATE.saveToFile();
  LOG_DBG("SLP", "Randomly loading: %s (%u candidates)", outPath.c_str(), candidateCount);
  return true;
}

bool pickFromFolder(const char* dirPath, std::string& outPath) {
  FsFile dir;
  if (!openSleepDir(dirPath, dir)) return false;
  const bool picked = pickSleepImage(dir, dirPath, outPath);
  dir.close();
  return picked;
}
}  // namespace

bool pickPath(std::string& outPath) {
  // A folder the user picked in the file browser wins over everything else.
  // Missing, or holding no images: the built-in lookup below still applies, so
  // a removed folder cannot leave the device without its sleep image.
  const std::string& preferredDir = APP_STATE.preferredSleepFolderPath;
  if (!preferredDir.empty()) {
    if (pickFromFolder(preferredDir.c_str(), outPath)) return true;
    LOG_INF("SLP", "No usable image in chosen sleep folder, falling back: %s", preferredDir.c_str());
  }

  // /sleep.bmp on the root of the SD card takes priority over the default
  // sleep folders.
  if (Storage.exists(ROOT_SLEEP_IMAGE)) {
    outPath = ROOT_SLEEP_IMAGE;
    return true;
  }

  // /.sleep (preferred) or /sleep. Only the first that exists is used: an
  // empty /.sleep does not fall through to /sleep.
  FsFile probe;
  const char* defaultDir = nullptr;
  if (openSleepDir(DEFAULT_SLEEP_DIR_HIDDEN, probe)) {
    defaultDir = DEFAULT_SLEEP_DIR_HIDDEN;
  } else if (openSleepDir(DEFAULT_SLEEP_DIR, probe)) {
    defaultDir = DEFAULT_SLEEP_DIR;
  }
  if (probe) probe.close();
  return defaultDir != nullptr && pickFromFolder(defaultDir, outPath);
}

}  // namespace SleepImage
