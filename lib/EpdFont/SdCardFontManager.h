#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;
struct SdCardFontFileInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the font file matching fontSizeEnum (SMALL=0 .. EXTRA_LARGE=3) by
  // ordinal position in the family's sorted size list. Only one .cpfont file
  // is loaded; other sizes remain on disk. This keeps resident interval +
  // kern/ligature tables to one size's worth of memory.
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

  // The loaded sizes of the family, smallest first, for EPUB headings and CSS font-size:
  // the body size plus the extra sizes loadFamily() could afford. Unused slots are 0.
  void ladder(int* fontIds, uint8_t* points, size_t slots) const;

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);
  // Loads one file and registers it with the renderer; returns its font id, 0 on failure.
  int loadFile(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file, GfxRenderer& renderer);
  void loadExtraSizes(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};
