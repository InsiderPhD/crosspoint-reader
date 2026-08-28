#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffers + group scratch). For heap-critical work
  // (section builds, TLS sessions) that needs every byte back.
  void clearCache();

  // Free only the per-page glyph buffers, keeping the decompression scratch
  // allocated. Re-malloc'ing that ~3KB block on every page turn is what fails
  // once the BLE stack is resident and free heap sits near 10KB, so it is held
  // for the reading session and released only by clearCache().
  void clearPageCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
    uint16_t allocFailures = 0;    // glyph buffers this page could not allocate (missing glyphs)
  };
  // Per-page font-cache profiling. Ten lines of serial per page turn drowns
  // everything else out, so the output is compiled in only under
  // -DFONT_CACHE_STATS (put it in platformio.local.ini when profiling); the
  // counters themselves always run and always reset.
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Group scratch / hot group: one grow-only buffer, shared by prewarmCache()'s
  // per-group decompression and getBitmap()'s non-prewarmed fallback path. It
  // holds the last decompressed group (byte-aligned) so the fallback can hit it
  // without re-inflating; individual glyphs are compacted on demand into hotGlyphBuf.
  // malloc'd, NOT std::vector: resize() failure throws bad_alloc, which is an
  // instant abort() under -fno-exceptions — and this path runs exactly when the
  // heap is at its tightest (a glyph the prewarm couldn't fit). Allocation
  // failure here must degrade to a missing glyph, not a reboot.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  uint8_t* hotGroup = nullptr;
  uint32_t hotGroupCap = 0;  // bytes allocated at hotGroup

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphCap = 0;  // bytes allocated at hotGlyphBuf

  // Log throttle: at heap exhaustion the hot-group path runs (and fails) for
  // EVERY glyph on the page — thousands of unguarded LOG_ERR lines per render.
  // Log only when the failing group changes; reset on success or clearCache().
  const EpdFontData* lastAllocFailFont = nullptr;
  uint16_t lastAllocFailGroup = UINT16_MAX;

  void freePageBuffer();
  void freeHotGroup();
  // Grow-only accessor for the shared group scratch. Returns nullptr on OOM.
  uint8_t* acquireGroupScratch(uint32_t size);
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
