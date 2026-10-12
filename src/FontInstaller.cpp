#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>  // strcasecmp

#include <cctype>
#include <cstring>
#include <initializer_list>

#include "CrossPointSettings.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

bool FontInstaller::isValidFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path traversal
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidCpfontFilename(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Reject path separators / traversal up front. Anything that could escape
  // the family directory or refer to a different one is a hard reject.
  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  // Must end with ".cpfont" exactly.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  size_t nameLen = strlen(name);
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;

  // Basename (before .cpfont) must be alphanumeric + hyphen + underscore only.
  // No additional dots — keeps stray "Foo.cpfont.tmp"-style names out.
  size_t baseLen = nameLen - kExtLen;
  for (size_t i = 0; i < baseLen; ++i) {
    char c = name[i];
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

// Copies `name` to outBuf with every basename character that is not
// alphanumeric/hyphen/underscore (spaces, %, etc.) replaced by '_', keeping its
// last extLen characters (the extension, already checked by the caller) as
// they are. This also neutralises any slash, backslash or '.' in the basename,
// so path traversal cannot survive sanitisation.
static bool sanitizeFontFilename(const char* name, const size_t extLen, char* outBuf, const size_t outBufSize) {
  const size_t nameLen = strlen(name);
  if (nameLen <= extLen) return false;  // empty basename
  // Need room for the sanitised basename + extension + NUL.
  if (nameLen + 1 > outBufSize) return false;

  const size_t baseLen = nameLen - extLen;
  for (size_t i = 0; i < baseLen; ++i) {
    const char c = name[i];
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    outBuf[i] = ok ? c : '_';
  }
  memcpy(outBuf + baseLen, name + baseLen, extLen);
  outBuf[nameLen] = '\0';
  return true;
}

bool FontInstaller::sanitizeCpfontFilename(const char* name, char* outBuf, size_t outBufSize) {
  if (name == nullptr || outBuf == nullptr || outBufSize == 0) return false;

  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(name);
  // Must already carry the ".cpfont" extension (case-sensitive, matching the validator).
  if (nameLen <= kExtLen) return false;
  if (strcmp(name + nameLen - kExtLen, kExt) != 0) return false;
  return sanitizeFontFilename(name, kExtLen, outBuf, outBufSize);
}

bool FontInstaller::hasVectorFontExtension(const char* name) {
  if (name == nullptr) return false;
  static constexpr size_t kExtLen = 4;  // ".ttf" / ".otf"
  const size_t nameLen = strlen(name);
  if (nameLen < kExtLen) return false;
  const char* ext = name + nameLen - kExtLen;
  return strcasecmp(ext, ".ttf") == 0 || strcasecmp(ext, ".otf") == 0;
}

bool FontInstaller::sanitizeVectorFontFilename(const char* name, char* outBuf, size_t outBufSize) {
  if (outBuf == nullptr || outBufSize == 0 || !hasVectorFontExtension(name)) return false;
  return sanitizeFontFilename(name, 4, outBuf, outBufSize);
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  // Reuse the family's existing root if installed; otherwise pick the
  // default-write root (hidden if no roots exist yet).
  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();

  if (!Storage.exists(root)) {
    if (!Storage.mkdir(root)) {
      LOG_ERR("FONT", "Failed to create fonts dir: %s", root);
      return false;
    }
  }

  char dirPath[160];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateCpfontFile(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  uint8_t magic[CPFONT_MAGIC_LEN];
  size_t bytesRead = file.read(magic, CPFONT_MAGIC_LEN);
  file.close();

  if (bytesRead < CPFONT_MAGIC_LEN) {
    LOG_ERR("FONT", "File too small: %s (%zu bytes)", path, bytesRead);
    return false;
  }

  if (memcmp(magic, "CPFONT\0\0", CPFONT_MAGIC_LEN) != 0) {
    LOG_ERR("FONT", "Bad magic in: %s", path);
    return false;
  }

  return true;
}

void FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize) {
  // Use the same root selection as ensureFamilyDir: existing install dir wins,
  // otherwise the default-write root.
  const char* root = SdCardFontRegistry::findFamilyRoot(family);
  if (!root) root = SdCardFontRegistry::defaultWriteRoot();
  snprintf(outBuf, outBufSize, "%s/%s/%s", root, family, filename);
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    return Error::INVALID_FAMILY_NAME;
  }

  // A family may exist in either root (or, edge case, both). Remove from both.
  const char* roots[] = {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE};
  bool removedAny = false;
  bool sawAny = false;
  for (const char* root : roots) {
    char dirPath[160];
    snprintf(dirPath, sizeof(dirPath), "%s/%s", root, familyName);
    if (!Storage.exists(dirPath)) continue;
    sawAny = true;
    if (!Storage.removeDir(dirPath)) {
      LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
      return Error::SD_WRITE_ERROR;
    }
    removedAny = true;
  }

#if CROSSPOINT_VECTOR_FONTS
  // A vector family can also be a loose file in a fonts root. isValidFamilyName()
  // above limits the name to [A-Za-z0-9_-], so it cannot leave the root.
  for (const char* root : roots) {
    for (const char* ext : {".ttf", ".otf"}) {
      char filePath[160];
      snprintf(filePath, sizeof(filePath), "%s/%s%s", root, familyName, ext);
      if (!Storage.exists(filePath)) continue;
      sawAny = true;
      if (!Storage.remove(filePath)) {
        LOG_ERR("FONT", "Failed to remove font file: %s", filePath);
        return Error::SD_WRITE_ERROR;
      }
      removedAny = true;
    }
  }
#endif

  if (!sawAny) {
    LOG_DBG("FONT", "Family not found in any fonts root: %s", familyName);
    return Error::OK;  // Already gone
  }
  (void)removedAny;

  // If this was the active font, clear the setting
  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("FONT", "Cleared active SD font (deleted family: %s)", familyName);
  }

  return Error::OK;
}

void FontInstaller::refreshRegistry() { registry_.discover(); }

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return registry_.findFamily(familyName) != nullptr;
}
