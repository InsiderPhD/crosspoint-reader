#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>  // not used here; kept so includers relying on it transitively still build

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // Frees the per-page glyph buffers but keeps the ~3KB group-decompression
  // scratch allocated. Used between page renders; clearCache() is for the
  // heap-critical paths that need every byte back.
  void clearPageCache();
  // True if this page's prewarm/render could not allocate a glyph buffer — i.e.
  // characters are missing from what was just laid out. The caller decides what
  // heap to give back (on a tight-heap board, the BLE stack) and render again.
  bool glyphAllocFailed() const;
  // clearCache() plus each SD font's persistent advance cache — those survive
  // a normal clear by design (they amortize SD reads across passes) but can
  // hold 10KB+ after a reading session. Use before heap-critical work
  // (network transfers); the cost is re-indexing on the next SD-font render.
  void clearCachesDeep();
  // Frees every rebuildable font cache while keeping the fonts loaded: the
  // decompressor's page/group buffers and, per SD font, mini glyph/kern data,
  // ligature tables and the advance cache. For leaving the reader, so those blocks do
  // not pin the heap between reading sessions. Never call it between a prewarm
  // and its draw; the cost is a cold prewarm + re-index on the next SD-font use.
  void releaseSdFontCaches();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // Scan accumulator: every (font, resolved style, codepoint) seen during the
  // scan pass, packed into one uint32_t and deduplicated on insert. A page may
  // mix fonts (e.g. a <pre> block in the monospace font beside body text in the
  // reader font, plus the status bar's UI font); each (font, style) group is
  // prewarmed on its own with exactly the codepoints it draws, otherwise the
  // un-prewarmed font's glyphs load one at a time through the on-demand
  // overflow ring (slow SD I/O for SD-card fonts).
  //
  // Fixed storage, no heap: this replaced a per-render std::map<int, ScanEntry>
  // whose std::string grew by reallocation to the page's full text every render.
  // Cost is ~2.1KB of static DRAM (the manager is a global in main.cpp).
  // Fonts past MAX_SCAN_FONTS and codepoints past MAX_SCAN_CODEPOINTS are not
  // batched; they fall back to on-demand glyph loading during the draw pass.
  static constexpr uint8_t MAX_SCAN_FONTS = 6;
  static constexpr uint16_t MAX_SCAN_CODEPOINTS = 512;
  static constexpr uint8_t SCAN_STYLE_SHIFT = 21;  // Unicode needs 21 bits
  static constexpr uint8_t SCAN_FONT_SHIFT = SCAN_STYLE_SHIFT + 2;
  static constexpr uint32_t SCAN_CODEPOINT_MASK = (1U << SCAN_STYLE_SHIFT) - 1;
  static constexpr uint8_t SCAN_GROUP_COUNT = MAX_SCAN_FONTS * 4;

  // Maps a requested style to the one render will actually draw (a missing
  // italic resolves to regular, etc.) so two requested styles that share glyph
  // data land in one group instead of being prewarmed twice.
  uint8_t resolveScanStyle(int fontId, EpdFontFamily::Style style) const;
  void resetScan();

  // Font ids are FNV hashes cast to int and routinely negative, so occupancy is
  // tracked by scanFontCount_, not by a sentinel id.
  int scanFontIds_[MAX_SCAN_FONTS] = {};
  // +1: endScanAndPrewarm() re-encodes each group to UTF-8 in place and the last
  // group's NUL terminator may land one entry past its codepoints.
  uint32_t scanCodepoints_[MAX_SCAN_CODEPOINTS + 1] = {};
  uint16_t scanGroupCounts_[SCAN_GROUP_COUNT] = {};
  uint16_t scanCodepointCount_ = 0;
  uint8_t scanFontCount_ = 0;
  bool scanOverflowWarned_ = false;
};
