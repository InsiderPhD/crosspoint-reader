#pragma once

// Runtime TrueType / OpenType reader font: renders .ttf/.otf files as an
// ordinary EpdFontFamily (regular / bold / italic / bold-italic), so the
// renderer, layout, bionic reading, character spacing and clip selection use
// it through the same EpdFont calls as a built-in font.
//
// PSRAM boards only (CROSSPOINT_VECTOR_FONTS, see lib/DevicePolicy). Every
// allocation this class makes is requested from PSRAM explicitly: the font
// files (the rasteriser needs each file resident), the per-style glyph caches,
// the rasteriser's scratch, and its own state. Nothing lands in internal DRAM.
//
// STYLES. A family is built from up to four files, one per style role; only
// the regular file is required. A missing bold is synthesised by widening the
// regular outline's coverage, a missing italic by shearing it. Supplying real
// Bold / Italic files replaces the synthesised styles with the real designs.
//
// CACHES. Each style rasterises glyphs on first use (EpdFontData's
// glyphMissHandler) into a fixed-capacity PSRAM arena that is allocated on that
// style's first glyph and never moves, so EpdFontData::bitmap stays valid and
// the renderer needs no per-page prewarm for these fonts. A full arena is
// flushed whole. releaseCaches() frees every arena while keeping the font
// loaded; everything faults back in on demand.
//
// LIMITS of the stb_truetype backend: no hinting, no variable-font axes (the
// default instance is rendered), GPOS kerning limited to plain pair lookups,
// and ligatures only for fonts that map U+FB00..U+FB04 in their cmap.
// stb_truetype does not bounds-check font data, so a corrupt file can crash.

#include "EpdFontData.h"

#if CROSSPOINT_VECTOR_FONTS

#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "EpdFontFamily.h"

class TtfEpdFont {
 public:
  enum Style : uint8_t { REGULAR = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, STYLE_COUNT = 4 };

  // Largest single font file, and largest family (all style files together),
  // this class will hold resident. The registry hides files over the per-file
  // cap so an unloadable family is never offered in the font picker.
  static constexpr uint32_t MAX_FILE_BYTES = 3u * 1024u * 1024u;
  static constexpr uint32_t MAX_FAMILY_BYTES = 4u * 1024u * 1024u;

  enum class LoadResult : uint8_t {
    OK,
    // Out of PSRAM or an SD read error: worth retrying later.
    UNAVAILABLE,
    // The regular file is not a font stb_truetype can parse: retrying is futile.
    INVALID_FONT,
  };

  // Returns nullptr when PSRAM cannot fund the object.
  static TtfEpdFont* create();
  static void destroy(TtfEpdFont* font);

  TtfEpdFont(const TtfEpdFont&) = delete;
  TtfEpdFont& operator=(const TtfEpdFont&) = delete;

  // Read one style's file into PSRAM. Call before load(); REGULAR is required.
  // A failed BOLD / ITALIC / BOLD_ITALIC source is not fatal: that style is
  // synthesised from the best remaining source instead.
  LoadResult addSource(uint8_t style, const char* path);

  // (Re)build the four faces at pointSize, measured like the .cpfont converter
  // and the built-in fonts: points at 150 DPI. Drops every cached glyph.
  LoadResult load(uint8_t pointSize);

  EpdFontFamily family() const;

  // FNV-1a over each source file's length, head and tail. Feeds the renderer
  // font id, which the section cache stores as its font identity, so replacing
  // a font file on the card invalidates the sections laid out with the old one.
  uint32_t contentHash() const;

  // Free every glyph arena, glyph table and kern cache (PSRAM) while keeping
  // the font files and faces loaded.
  void releaseCaches();

  // releaseCaches() on every live TtfEpdFont. Called by FontCacheManager on the
  // same transitions that release the SD font caches (leaving the reader,
  // before large downloads).
  static void releaseAllCaches();

 private:
  TtfEpdFont() = default;
  ~TtfEpdFont() = default;

  struct Impl;
  Impl* impl_ = nullptr;
  TtfEpdFont* nextLive_ = nullptr;
  static TtfEpdFont* liveHead_;
};

#endif  // CROSSPOINT_VECTOR_FONTS
