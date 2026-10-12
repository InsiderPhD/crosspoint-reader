#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "VectorFontSupport.h"

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/<root>/<Family>/<Family>_<size>.cpfont"
                      // where <root> is "/.fonts" (preferred, hidden) or "/fonts" (visible).
                      // e.g. "/.fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14 (0 for a vector font, which has no fixed size)
  uint8_t style;      // .cpfont: always 0 (all 4 styles bundled in one file).
                      // Vector family: the style ROLE of this file, 0=regular,
                      // 1=bold, 2=italic, 3=bold-italic.
};

struct SdCardFontFamilyInfo {
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;
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

  // Scan SD card, populate families_. Returns true if any families found.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
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
