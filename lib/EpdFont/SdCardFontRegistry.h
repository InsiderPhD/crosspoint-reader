#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "VectorFontSupport.h"

struct SdCardFontFileInfo {
  // Filename, stored only when SdCardFontFamilyInfo::filePath() cannot rebuild
  // it: a .cpfont not named "<Family>_<size>.cpfont", or any vector file. Null
  // for the canonical name, so downloaded families keep no per-file heap string
  // and the entry is two pointers wide (8 bytes on the ESP32-C3). It used to be
  // a full std::string path per file, resident from boot.
  std::unique_ptr<char[]> name;
  uint8_t pointSize;  // parsed from filename: 14 (0 for a vector font, which has no fixed size)
  uint8_t style;      // .cpfont: always 0 (all 4 styles bundled in one file).
                      // Vector family: the style ROLE of this file, 0=regular,
                      // 1=bold, 2=italic, 3=bold-italic.
};

struct SdCardFontFamilyInfo {
  // Buffer size for filePath(). Discovery drops files whose path would not
  // fit, so every registered file opens through a buffer of this size.
  static constexpr size_t PATH_BUFFER_SIZE = 160;

  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
  // SdCardFontRegistry::FONTS_DIR_HIDDEN or FONTS_DIR_VISIBLE: static storage,
  // never freed, so a pointer is enough.
  const char* root = nullptr;
  bool loose = false;  // vector file directly under `root`, no family directory
#if CROSSPOINT_VECTOR_FONTS
  // true for a TrueType/OpenType family (.ttf/.otf) rendered at any size by
  // TtfEpdFont: `files` then holds one entry per style role, pointSize 0.
  // false = a directory of pre-rasterised .cpfont files.
  bool vector = false;
#endif
  // Always false on builds without vector font support (no PSRAM): the registry
  // never lists .ttf/.otf files there.
  bool isVector() const {
#if CROSSPOINT_VECTOR_FONTS
    return vector;
#else
    return false;
#endif
  }

  // Writes the file's SD path, e.g. "/.fonts/NotoSansCJK/NotoSansCJK_14.cpfont".
  // Returns false when `outSize` is too small; `out` is then truncated.
  bool filePath(const SdCardFontFileInfo& file, char* out, size_t outSize) const;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;
};

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Two top-level roots are scanned at discovery time. Hidden is preferred
  // when creating new installs; both are read from if present.
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";

  // Returns the existing root for `familyName` (the one that contains
  // /<root>/<familyName>/), or nullptr if the family is not installed in
  // either root. Used by writers to keep re-installs in their existing dir.
  static const char* findFamilyRoot(const char* familyName);

  // Returns the root path that should be used when creating a brand-new
  // family on disk (no prior install): the existing root if exactly one of
  // the two roots exists, otherwise the hidden root.
  static const char* defaultWriteRoot();

  // Scan SD card, populate families_. Invalidates pointers and references
  // obtained from getFamilies()/findFamily(). Returns true if any families found.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  // True when `filename` is exactly "<family>_<size>.cpfont", the name
  // filePath() rebuilds without a stored string.
  static bool isCanonicalCpfontName(const char* filename, const std::string& family, uint8_t size);
#if CROSSPOINT_VECTOR_FONTS
  // Matches a .ttf/.otf filename (case-insensitive) and returns the length of
  // the name without its extension.
  static bool parseVectorFontName(const char* filename, size_t& baseLen);
  // Reduce a folder's vector files to at most one per style role, chosen by
  // each face's own weight and italic flag (filename tokens when unreadable).
  static void assignVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files);
#endif
  static void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  // Scan one root (e.g. "/.fonts"), append families to `out`, dedup by name.
  static void scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out);
};
