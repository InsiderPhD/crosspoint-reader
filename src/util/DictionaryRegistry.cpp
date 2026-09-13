#include "DictionaryRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "StringUtils.h"

namespace DictionaryRegistry {
namespace {

// Dictionaries are looked up in both roots, in order. The hidden variant
// lets users keep the folder out of the file browser (hidden by default,
// see FileBrowserActivity's showHiddenFiles check).
constexpr const char* DICT_ROOTS[] = {"/dictionaries", "/.dictionaries"};

// Find the single .idx stem inside one dictionary folder. Returns false when
// the folder holds no .idx or more than one distinct stem (ambiguous).
bool findStem(const char* folderPath, std::string& stemOut) {
  auto dir = Storage.open(folderPath);
  if (!dir || !dir.isDirectory()) return false;

  dir.rewindDirectory();
  char name[128];
  char foundStem[128];
  foundStem[0] = '\0';
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    // Skip macOS metadata files (AppleDouble resource forks)
    if (entry.isDirectory() || strncmp(name, "._", 2) == 0) continue;

    const size_t len = strlen(name);
    if (len <= 4 || strcmp(name + len - 4, ".idx") != 0) continue;

    name[len - 4] = '\0';
    if (foundStem[0] != '\0' && strcmp(foundStem, name) != 0) {
      LOG_DBG("DREG", "Skipping %s: multiple index stems found", folderPath);
      return false;
    }
    strncpy(foundStem, name, sizeof(foundStem) - 1);
    foundStem[sizeof(foundStem) - 1] = '\0';
  }

  if (foundStem[0] == '\0') return false;

  // Require dictionary data next to the index, so folders holding only an
  // .idx never surface as selectable dictionaries that fail at lookup time.
  const std::string base = std::string(folderPath) + "/" + foundStem;
  if (!Storage.exists((base + ".dict").c_str()) && !Storage.exists((base + ".dict.dz").c_str())) {
    LOG_DBG("DREG", "Skipping %s: no .dict or .dict.dz", folderPath);
    return false;
  }

  stemOut = foundStem;
  return true;
}

}  // namespace

void discover(std::vector<DictionaryEntry>& out) {
  out.clear();
  out.reserve(8);

  for (const char* dictRoot : DICT_ROOTS) {
    auto rootDir = Storage.open(dictRoot);
    if (!rootDir || !rootDir.isDirectory()) {
      LOG_DBG("DREG", "No %s directory on SD card", dictRoot);
      continue;
    }

    rootDir.rewindDirectory();
    char name[128];
    for (auto entry = rootDir.openNextFile(); entry; entry = rootDir.openNextFile()) {
      entry.getName(name, sizeof(name));
      if (!entry.isDirectory() || name[0] == '.') continue;

      std::string folderPath = std::string(dictRoot) + "/" + name;
      std::string stem;
      if (!findStem(folderPath.c_str(), stem)) continue;

      DictionaryEntry e;
      e.name = name;
      e.stem = std::move(stem);
      out.push_back(std::move(e));
      LOG_DBG("DREG", "Found dictionary: %s", name);
    }
  }

  // Case-insensitive sort by folder name (matches FileBrowserActivity ordering).
  std::sort(out.begin(), out.end(), [](const DictionaryEntry& a, const DictionaryEntry& b) {
    return StringUtils::asciiCaseCmp(a.name.c_str(), b.name.c_str()) < 0;
  });
}

bool resolveBasePath(const char* folderName, std::string& basePathOut) {
  if (!folderName || folderName[0] == '\0') return false;
  // folderName is persisted in the settings JSON: reject separators and dot
  // prefixes so a crafted value cannot escape the dictionary roots.
  if (folderName[0] == '.' || strpbrk(folderName, "/\\") != nullptr) return false;

  for (const char* dictRoot : DICT_ROOTS) {
    std::string folderPath = std::string(dictRoot) + "/" + folderName;
    std::string stem;
    if (!findStem(folderPath.c_str(), stem)) continue;
    basePathOut = folderPath + "/" + stem;
    return true;
  }
  return false;
}

// --- Install support --------------------------------------------------------

namespace {

// Shared character rule for both validators: the set that can appear in a
// catalog-supplied path component. Deliberately narrow — every one of these is
// concatenated into an SD path.
bool isSafePathChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

// Length caps. A folder name has to round-trip through
// CrossPointSettings::dictionaryName (char[32]), so a longer one would install
// but come back truncated and unresolvable — reject it up front instead.
// Filenames only have to fit the path Dictionary.h budgets (~137 chars for
// folder + stem before the longest ".dict.dz" suffix).
constexpr size_t MAX_NAME_LEN = 31;
constexpr size_t MAX_FILENAME_LEN = 64;

bool isValidComponent(const char* name, const size_t maxLen) {
  if (!name || name[0] == '\0' || name[0] == '.') return false;
  size_t len = 0;
  for (const char* p = name; *p; ++p, ++len) {
    if (!isSafePathChar(*p)) return false;
  }
  return len <= maxLen;
}

}  // namespace

bool isValidName(const char* name) {
  // A folder name additionally carries no dots: it is compared against
  // SETTINGS.dictionaryName and shown in the picker.
  if (!isValidComponent(name, MAX_NAME_LEN)) return false;
  return strchr(name, '.') == nullptr;
}

bool isValidFileName(const char* name) { return isValidComponent(name, MAX_FILENAME_LEN); }

bool ensureInstallDir(const char* name, std::string& dirOut) {
  if (!isValidName(name)) {
    LOG_ERR("DREG", "Rejected dictionary name: %s", name ? name : "(null)");
    return false;
  }

  // An existing install keeps its root, so updating a dictionary the user
  // moved to /.dictionaries does not silently create a second copy in the
  // visible root that would then shadow it in discover()'s root order.
  for (const char* dictRoot : DICT_ROOTS) {
    std::string existing = std::string(dictRoot) + "/" + name;
    if (Storage.exists(existing.c_str())) {
      dirOut = std::move(existing);
      return true;
    }
  }

  // New install: the visible root, created on demand (a fresh card has neither).
  dirOut = std::string(DICT_ROOTS[0]) + "/" + name;
  if (!Storage.mkdir(dirOut.c_str())) {
    LOG_ERR("DREG", "Failed to create %s", dirOut.c_str());
    return false;
  }
  return true;
}

bool removeDictionary(const char* name) {
  if (!isValidName(name)) return false;

  bool foundAny = false;
  for (const char* dictRoot : DICT_ROOTS) {
    const std::string folderPath = std::string(dictRoot) + "/" + name;
    if (!Storage.exists(folderPath.c_str())) continue;
    foundAny = true;
    // removeDir is recursive in HalStorage; the dictionary folder is flat, but
    // a stray AppleDouble subfolder must not block the delete.
    if (!Storage.removeDir(folderPath.c_str())) {
      LOG_ERR("DREG", "Failed to remove %s", folderPath.c_str());
      return false;
    }
    LOG_DBG("DREG", "Removed dictionary %s", folderPath.c_str());
  }
  return foundAny;
}

}  // namespace DictionaryRegistry
