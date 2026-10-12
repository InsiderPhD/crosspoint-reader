#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>  // strcasecmp

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if CROSSPOINT_VECTOR_FONTS
#include "TtfEpdFont.h"

namespace {

// CrossPointSettings::sdFontFamilyName is char[32]; a longer family name could
// be listed but never selected.
constexpr size_t MAX_VECTOR_FAMILY_NAME = 31;
// Style files considered per family folder (Thin..Black, upright and italic).
constexpr size_t MAX_VECTOR_FILES_PER_FAMILY = 18;

uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t readBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// Style role from filename tokens: "bold" (also semibold, extrabold) sets the
// bold bit, "italic"/"oblique" the italic bit.
uint8_t styleFromName(const char* name, const size_t length) {
  bool bold = false;
  bool italic = false;
  for (size_t i = 0; i < length; i++) {
    if (length - i >= 4 && strncasecmp(name + i, "bold", 4) == 0) bold = true;
    if (length - i >= 6 && strncasecmp(name + i, "italic", 6) == 0) italic = true;
    if (length - i >= 7 && strncasecmp(name + i, "oblique", 7) == 0) italic = true;
  }
  return static_cast<uint8_t>((bold ? 1 : 0) | (italic ? 2 : 0));
}

// Reads a face's design weight and italic flag from its OS/2 table: a few
// small reads (header, table directory entries, 64 bytes of OS/2), no parsing
// of the font proper. Returns false, leaving the outputs untouched, when the
// file has no readable OS/2 table.
bool inspectVectorFace(const char* path, uint16_t& weight, bool& italic) {
  FsFile file;
  if (!Storage.openFileForRead("SDREG", path, file)) return false;
  bool found = false;
  uint8_t buffer[64];
  uint32_t fontOffset = 0;
  bool headerOk = file.read(buffer, 16) == 16;
  if (headerOk && memcmp(buffer, "ttcf", 4) == 0) {
    // Collection: describe its first font, the one TtfEpdFont renders.
    fontOffset = readBe32(buffer + 12);
    headerOk = file.seek(fontOffset) && file.read(buffer, 12) == 12;
  }
  if (headerOk && file.seek(fontOffset + 12)) {
    const uint16_t tableCount = std::min<uint16_t>(readBe16(buffer + 4), 64);
    uint32_t tableOffset = 0;
    uint32_t tableLength = 0;
    for (uint16_t i = 0; i < tableCount; i++) {
      if (file.read(buffer, 16) != 16) break;
      if (memcmp(buffer, "OS/2", 4) == 0) {
        tableOffset = readBe32(buffer + 8);
        tableLength = readBe32(buffer + 12);
        break;
      }
    }
    // usWeightClass at byte 4, fsSelection at byte 62 (bit 0 italic, bit 9 oblique).
    if (tableLength >= 64 && file.seek(tableOffset) && file.read(buffer, 64) == 64) {
      const uint16_t faceWeight = readBe16(buffer + 4);
      if (faceWeight >= 1 && faceWeight <= 1000) {
        weight = faceWeight;
        italic = (readBe16(buffer + 62) & 0x0201) != 0;
        found = true;
      }
    }
  }
  file.close();
  return found;
}

}  // namespace
#endif  // CROSSPOINT_VECTOR_FONTS

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; scanDirectory() defends against accidental
  // (pointSize, style) collisions in that scenario.
  style = 0;
  return true;
}

#if CROSSPOINT_VECTOR_FONTS

bool SdCardFontRegistry::parseVectorFontName(const char* filename, size_t& baseLen) {
  static constexpr const char* kExts[] = {".ttf", ".otf"};
  const size_t nameLen = strlen(filename);
  for (const char* ext : kExts) {
    const size_t extLen = strlen(ext);
    if (nameLen <= extLen) continue;
    if (strcasecmp(filename + nameLen - extLen, ext) == 0) {
      baseLen = nameLen - extLen;
      return true;
    }
  }
  return false;
}

void SdCardFontRegistry::assignVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files) {
  // Pick the four roles by design weight, never by directory order: the upright
  // face nearest 400 is regular and the nearest 700 is bold, likewise for the
  // italics. A Light/Regular/Medium/Bold/Black family therefore always resolves
  // to Regular + Bold, whichever file the card happens to enumerate first.
  struct Candidate {
    size_t index;  // into files
    uint16_t weight;
    bool italic;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(files.size());
  for (size_t i = 0; i < files.size(); i++) {
    // files[i].style holds the filename-derived role until the face answers.
    Candidate candidate{i, static_cast<uint16_t>((files[i].style & 1) ? 700 : 400), (files[i].style & 2) != 0};
    if (files.size() > 1) inspectVectorFace(files[i].path.c_str(), candidate.weight, candidate.italic);
    candidates.push_back(candidate);
  }

  // Nearest to `target` within the upright or italic group; ties go to the
  // lower weight, then the smaller path. `exclude` stops bold re-picking the
  // regular file.
  const auto pick = [&](const bool italic, const int target, const Candidate* exclude) -> const Candidate* {
    const Candidate* best = nullptr;
    for (const auto& candidate : candidates) {
      if (candidate.italic != italic || &candidate == exclude) continue;
      if (!best) {
        best = &candidate;
        continue;
      }
      const int distance = std::abs(static_cast<int>(candidate.weight) - target);
      const int bestDistance = std::abs(static_cast<int>(best->weight) - target);
      if (distance < bestDistance ||
          (distance == bestDistance &&
           (candidate.weight < best->weight ||
            (candidate.weight == best->weight && files[candidate.index].path < files[best->index].path)))) {
        best = &candidate;
      }
    }
    return best;
  };

  const Candidate* regular = pick(false, 400, nullptr);
  if (!regular) {
    // Italic-only folder: the italic nearest 400 anchors the family.
    regular = pick(true, 400, nullptr);
    if (!regular) {
      files.clear();
      return;
    }
    LOG_DBG("SDREG", "No upright face in %s; using %s as regular", dirPath, files[regular->index].path.c_str());
  }
  // A "bold" no heavier than the regular pick would render identically, so the
  // synthesised bold is the better answer. Same rule for bold-italic.
  const Candidate* bold = regular->italic ? nullptr : pick(false, 700, regular);
  if (bold && bold->weight <= regular->weight) bold = nullptr;
  const Candidate* italic = regular->italic ? nullptr : pick(true, 400, nullptr);
  const Candidate* lighterItalic = italic ? italic : regular;
  const Candidate* boldItalic = pick(true, 700, lighterItalic);
  if (boldItalic && boldItalic->weight <= lighterItalic->weight) boldItalic = nullptr;

  std::vector<SdCardFontFileInfo> selected;
  selected.reserve(4);
  const auto add = [&](const Candidate* candidate, const uint8_t role) {
    if (!candidate) return;
    SdCardFontFileInfo info = files[candidate->index];
    info.style = role;
    selected.push_back(std::move(info));
  };
  add(regular, 0);
  add(bold, 1);
  add(italic, 2);
  add(boldItalic, 3);
  files = std::move(selected);
}

#endif  // CROSSPOINT_VECTOR_FONTS

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

#if CROSSPOINT_VECTOR_FONTS
  // Vector candidates are collected in the same pass (the handle is
  // forward-only) and used only if the folder holds no .cpfont file: a
  // pre-rasterised family is the more specific artifact.
  std::vector<SdCardFontFileInfo> vectorFiles;
  const bool vectorNameFits = family.name.size() <= MAX_VECTOR_FAMILY_NAME;
#endif

  char nameBuffer[128];
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
#if CROSSPOINT_VECTOR_FONTS
    const size_t entrySize = entry.size();
#endif
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) {
#if CROSSPOINT_VECTOR_FONTS
      size_t baseLen = 0;
      if (vectorNameFits && parseVectorFontName(nameBuffer, baseLen)) {
        if (entrySize == 0 || entrySize > TtfEpdFont::MAX_FILE_BYTES) {
          LOG_ERR("SDREG", "%s/%s: %u bytes is outside the supported font size, skipping", dirPath, nameBuffer,
                  static_cast<unsigned>(entrySize));
        } else if (vectorFiles.size() < MAX_VECTOR_FILES_PER_FAMILY) {
          if (vectorFiles.empty()) vectorFiles.reserve(4);
          SdCardFontFileInfo info;
          info.path = std::string(dirPath) + "/" + nameBuffer;
          info.pointSize = 0;
          info.style = styleFromName(nameBuffer, baseLen);
          vectorFiles.push_back(std::move(info));
        }
      }
#endif
      continue;
    }

    // Reject duplicate (pointSize, style) entries in the same family. With
    // v4's bundle-everything design parseFilename always returns style=0, so
    // two files at the same size in the same family would silently shadow
    // each other in findFile(). Skip the duplicate and warn.
    bool duplicate = false;
    for (const auto& existing : family.files) {
      if (existing.pointSize == size && existing.style == style) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
      continue;
    }

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = size;
    info.style = style;
    family.files.push_back(std::move(info));
  }

#if CROSSPOINT_VECTOR_FONTS
  if (family.files.empty() && !vectorFiles.empty()) {
    dir.close();  // assignVectorStyles opens the candidates
    assignVectorStyles(dirPath, vectorFiles);
    if (!vectorFiles.empty()) {
      family.vector = true;
      family.files = std::move(vectorFiles);
    }
  }
#endif
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
void SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  FsFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return;
  }

  const auto familyExists = [&out](const char* name) {
    for (const auto& fam : out) {
      if (fam.name == name) return true;
    }
    return false;
  };

  char nameBuffer[128];
  while (true) {
    FsFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      // De-dup by family name across roots.
      if (familyExists(nameBuffer)) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      SdCardFontRegistry::scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
        out.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", out.back().name.c_str(),
                static_cast<int>(out.back().files.size()), rootPath);
      }
    } else {
#if CROSSPOINT_VECTOR_FONTS
      // A loose TrueType/OpenType file directly under the root (e.g.
      // /fonts/Literata.ttf) is a one-file family named after the file.
      entry.getName(nameBuffer, sizeof(nameBuffer));
      const size_t entrySize = entry.size();
      entry.close();
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
      size_t baseLen = 0;
      if (!parseVectorFontName(nameBuffer, baseLen)) continue;
      if (baseLen > MAX_VECTOR_FAMILY_NAME || entrySize == 0 || entrySize > TtfEpdFont::MAX_FILE_BYTES) {
        LOG_ERR("SDREG", "Skipping %s/%s (name over %u characters or unsupported size %u)", rootPath, nameBuffer,
                static_cast<unsigned>(MAX_VECTOR_FAMILY_NAME), static_cast<unsigned>(entrySize));
        continue;
      }

      SdCardFontFamilyInfo family;
      family.name.assign(nameBuffer, baseLen);
      if (familyExists(family.name.c_str())) continue;
      family.vector = true;
      SdCardFontFileInfo info;
      info.path = std::string(rootPath) + "/" + nameBuffer;
      info.pointSize = 0;
      info.style = 0;
      family.files.push_back(std::move(info));
      out.push_back(std::move(family));
      LOG_DBG("SDREG", "Found vector font: %s in %s", out.back().name.c_str(), rootPath);
#else
      entry.close();
#endif
    }
  }
}

bool SdCardFontRegistry::discover() {
  families_.clear();
  families_.reserve(MAX_SD_FAMILIES);

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  scanRoot(FONTS_DIR_HIDDEN, families_);
  scanRoot(FONTS_DIR_VISIBLE, families_);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
