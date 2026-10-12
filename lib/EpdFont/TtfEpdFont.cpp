#include "TtfEpdFont.h"

#if CROSSPOINT_VECTOR_FONTS

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace {

// Every allocation of this feature, including the rasteriser's own temporaries,
// is requested from PSRAM by capability rather than left to the size threshold
// in CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL: the small ones (outline vertices,
// edge lists) would otherwise land in internal DRAM while a book is open.
void* ttfAlloc(const size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void ttfFree(void* block) { heap_caps_free(block); }

}  // namespace

// stb_truetype, private to this translation unit (STBTT_STATIC), so a second
// copy linked in from elsewhere cannot collide with it. Its asserts are
// compiled out: on this target an assert is abort(), and a font file is user
// data, not an invariant.
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), ttfAlloc(x))
#define STBTT_free(x, u) ((void)(u), ttfFree(x))
#define STBTT_assert(x) ((void)0)
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

// Bump when a change here alters glyph metrics for the same file and size: it
// is mixed into contentHash(), so sections laid out with the old metrics are
// rebuilt instead of being drawn with advances that no longer match.
constexpr uint32_t TTF_RENDER_REVISION = 1;

// Per-style cache, one PSRAM block allocated on the style's first glyph.
// 256KB holds ~1700 Latin glyphs at the largest reader size.
constexpr uint32_t ARENA_BYTES = 256u * 1024u;
constexpr uint16_t MAX_GLYPHS = 1536;
constexpr uint32_t KERN_SLOTS = 1024;  // power of two: direct-mapped

// EpdGlyph stores width and height in a byte each.
constexpr int MAX_GLYPH_DIMENSION = 255;
constexpr uint32_t SCRATCH_BYTES = static_cast<uint32_t>(MAX_GLYPH_DIMENSION) * MAX_GLYPH_DIMENSION;

// PSRAM the rest of the firmware keeps for itself (page caches, downloads,
// plugins): a font is refused rather than allowed to take the last of it.
constexpr size_t PSRAM_RESERVE_BYTES = 1024u * 1024u;

// Synthesised italic: x += ITALIC_SHEAR * y, about 11 degrees.
constexpr float ITALIC_SHEAR = 0.2f;

constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(uint32_t hash, const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

bool psramHasRoom(const size_t bytes) {
#ifdef SIMULATOR
  (void)bytes;  // host heap; the simulator reports no PSRAM unless built as a PSRAM board
  return true;
#else
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + PSRAM_RESERVE_BYTES;
#endif
}

uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint16_t readBe16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// stb_truetype trusts every offset in the file. Check the one structure whose
// damage is common in practice: the table directory of a truncated copy or
// upload. Returns the offset of the first font (0 unless a collection), or -1.
int validatedFontOffset(const uint8_t* bytes, const uint32_t length) {
  if (length < 12) return -1;
  uint32_t offset = 0;
  if (memcmp(bytes, "ttcf", 4) == 0) {
    if (length < 16 || readBe32(bytes + 8) == 0) return -1;
    offset = readBe32(bytes + 12);
    if (offset > length - 12) return -1;
  }
  const uint8_t* header = bytes + offset;
  static constexpr uint8_t TRUETYPE_MAGIC[4] = {0x00, 0x01, 0x00, 0x00};
  if (memcmp(header, TRUETYPE_MAGIC, 4) != 0 && memcmp(header, "OTTO", 4) != 0 && memcmp(header, "true", 4) != 0) {
    return -1;
  }
  const uint32_t tableCount = readBe16(header + 4);
  const uint64_t directoryEnd = static_cast<uint64_t>(offset) + 12 + static_cast<uint64_t>(tableCount) * 16;
  if (tableCount == 0 || directoryEnd > length) return -1;
  for (uint32_t i = 0; i < tableCount; i++) {
    const uint8_t* entry = header + 12 + i * 16;
    const uint64_t tableEnd = static_cast<uint64_t>(readBe32(entry + 8)) + readBe32(entry + 12);
    if (tableEnd > length) return -1;
  }
  return static_cast<int>(offset);
}

int16_t clampToShort(const float value) {
  if (value > 32767.0f) return 32767;
  if (value < -32768.0f) return -32768;
  return static_cast<int16_t>(lroundf(value));
}

}  // namespace

struct TtfEpdFont::Impl {
  struct Source {
    uint8_t* bytes = nullptr;
    uint32_t length = 0;
    stbtt_fontinfo info{};
    bool parsed = false;
    bool hasKerning = false;
  };

  struct KernEntry {
    uint32_t left;  // 0 = empty slot (a pair never has a zero codepoint)
    uint32_t right;
    int8_t value;
  };

  struct Face {
    Impl* owner = nullptr;
    uint8_t sourceIndex = 0;
    bool fauxBold = false;
    bool fauxItalic = false;
    float scale = 0.0f;

    // One PSRAM block: glyphs, codepoints, kern cache, slots, then the bitmap arena.
    void* cacheBlock = nullptr;
    bool cacheAllocFailed = false;
    EpdGlyph* glyphs = nullptr;      // append-only until a flush, so pointers into it stay valid
    uint32_t* codepoints = nullptr;  // sorted
    uint16_t* slots = nullptr;       // slots[i] = index into glyphs for codepoints[i]
    KernEntry* kern = nullptr;
    uint8_t* arena = nullptr;
    uint32_t arenaUsed = 0;
    uint16_t glyphCount = 0;

    EpdLigaturePair ligaturePairs[5] = {};
    EpdFontData data{};
    EpdFont font{&data};
  };

  Source sources[STYLE_COUNT];
  Face faces[STYLE_COUNT];
  uint8_t* scratch = nullptr;  // 8-bit coverage of the glyph being rasterised
  float pixelsPerEm = 0.0f;
  uint32_t familyBytes = 0;
  uint32_t contentHash = FNV_OFFSET;
  uint8_t boldPixels = 1;
  bool loaded = false;
  bool oversizeLogged = false;

  void resolveFaces();
  void setupFace(Face& face);
  bool ensureCache(Face& face);
  static void flushGlyphs(Face& face);
  void releaseFaceCache(Face& face);
  const EpdGlyph* faultGlyph(Face& face, uint32_t codepoint);
  bool rasterise(Face& face, int glyphIndex, EpdGlyph& out);
  int8_t kerning(Face& face, uint32_t leftCp, uint32_t rightCp);

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static int8_t kernThunk(void* ctx, uint32_t leftCp, uint32_t rightCp);
};

TtfEpdFont* TtfEpdFont::liveHead_ = nullptr;

TtfEpdFont* TtfEpdFont::create() {
  void* fontBlock = ttfAlloc(sizeof(TtfEpdFont));
  void* implBlock = ttfAlloc(sizeof(Impl));
  if (!fontBlock || !implBlock) {
    LOG_ERR("TTF", "PSRAM alloc failed: font state (%u bytes)", static_cast<unsigned>(sizeof(Impl)));
    ttfFree(fontBlock);
    ttfFree(implBlock);
    return nullptr;
  }
  auto* font = new (fontBlock) TtfEpdFont();
  font->impl_ = new (implBlock) Impl();
  for (auto& face : font->impl_->faces) face.owner = font->impl_;
  font->nextLive_ = liveHead_;
  liveHead_ = font;
  return font;
}

void TtfEpdFont::destroy(TtfEpdFont* font) {
  if (!font) return;
  for (TtfEpdFont** link = &liveHead_; *link; link = &(*link)->nextLive_) {
    if (*link == font) {
      *link = font->nextLive_;
      break;
    }
  }
  font->releaseCaches();
  for (auto& source : font->impl_->sources) {
    ttfFree(source.bytes);
    source.bytes = nullptr;
  }
  font->impl_->~Impl();
  ttfFree(font->impl_);
  font->~TtfEpdFont();
  ttfFree(font);
}

TtfEpdFont::LoadResult TtfEpdFont::addSource(const uint8_t style, const char* path) {
  if (style >= STYLE_COUNT || !path) return LoadResult::INVALID_FONT;
  Impl::Source& source = impl_->sources[style];
  if (source.bytes) return LoadResult::OK;  // one file per style role

  FsFile file;
  if (!Storage.openFileForRead("TTF", path, file)) {
    LOG_ERR("TTF", "Cannot open %s", path);
    return LoadResult::UNAVAILABLE;
  }
  const size_t length = file.size();
  if (length == 0 || length > MAX_FILE_BYTES || impl_->familyBytes + length > MAX_FAMILY_BYTES) {
    LOG_ERR("TTF", "%s: %u bytes is outside the supported size", path, static_cast<unsigned>(length));
    file.close();
    return LoadResult::INVALID_FONT;
  }
  if (!psramHasRoom(length)) {
    LOG_ERR("TTF", "Not enough PSRAM for %s (%u bytes)", path, static_cast<unsigned>(length));
    file.close();
    return LoadResult::UNAVAILABLE;
  }
  // stb_truetype reads the file through a pointer, so the whole file is
  // resident for as long as the font is loaded. PSRAM, capped above.
  auto* bytes = static_cast<uint8_t*>(ttfAlloc(length));
  if (!bytes) {
    LOG_ERR("TTF", "PSRAM alloc failed: %u bytes for %s", static_cast<unsigned>(length), path);
    file.close();
    return LoadResult::UNAVAILABLE;
  }
  // Chunked so the storage mutex is released between reads of a multi-MB file.
  static constexpr size_t READ_CHUNK = 64u * 1024u;
  size_t done = 0;
  while (done < length) {
    const size_t want = std::min(READ_CHUNK, length - done);
    const int got = file.read(bytes + done, want);
    if (got <= 0) break;
    done += static_cast<size_t>(got);
  }
  file.close();
  if (done != length) {
    LOG_ERR("TTF", "Short read on %s (%u of %u)", path, static_cast<unsigned>(done), static_cast<unsigned>(length));
    ttfFree(bytes);
    return LoadResult::UNAVAILABLE;
  }

  const int fontOffset = validatedFontOffset(bytes, static_cast<uint32_t>(length));
  source.info = stbtt_fontinfo{};
  if (fontOffset < 0 || !stbtt_InitFont(&source.info, bytes, fontOffset) || source.info.numGlyphs <= 0) {
    LOG_ERR("TTF", "%s is not a usable TrueType/OpenType font", path);
    ttfFree(bytes);
    return LoadResult::INVALID_FONT;
  }

  source.bytes = bytes;
  source.length = static_cast<uint32_t>(length);
  source.parsed = true;
  source.hasKerning = source.info.kern != 0 || source.info.gpos != 0;
  impl_->familyBytes += source.length;

  // Identity: which role, how long, and the head and tail of the file. The
  // table directory and the `head` table (checksum, modification time) sit in
  // the first KB of every sfnt, so an edited font hashes differently without
  // walking megabytes.
  static constexpr size_t HASH_SPAN = 1024;
  uint32_t hash = impl_->contentHash;
  const uint8_t identity[5] = {style, static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                               static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 24)};
  hash = fnv1a(hash, identity, sizeof(identity));
  hash = fnv1a(hash, bytes, std::min(HASH_SPAN, length));
  if (length > HASH_SPAN) hash = fnv1a(hash, bytes + length - HASH_SPAN, HASH_SPAN);
  impl_->contentHash = hash;

  LOG_DBG("TTF", "Loaded %s (%u KB, %d glyphs%s)", path, static_cast<unsigned>(length / 1024), source.info.numGlyphs,
          source.info.cff.size ? ", CFF" : "");
  return LoadResult::OK;
}

void TtfEpdFont::Impl::resolveFaces() {
  // Each style prefers its own file, then the closest real design with the
  // missing trait synthesised.
  const bool haveBold = sources[BOLD].parsed;
  const bool haveItalic = sources[ITALIC].parsed;
  const bool haveBoldItalic = sources[BOLD_ITALIC].parsed;

  faces[REGULAR].sourceIndex = REGULAR;
  faces[REGULAR].fauxBold = false;
  faces[REGULAR].fauxItalic = false;

  faces[BOLD].sourceIndex = haveBold ? BOLD : REGULAR;
  faces[BOLD].fauxBold = !haveBold;
  faces[BOLD].fauxItalic = false;

  faces[ITALIC].sourceIndex = haveItalic ? ITALIC : REGULAR;
  faces[ITALIC].fauxBold = false;
  faces[ITALIC].fauxItalic = !haveItalic;

  Face& boldItalic = faces[BOLD_ITALIC];
  if (haveBoldItalic) {
    boldItalic.sourceIndex = BOLD_ITALIC;
    boldItalic.fauxBold = false;
    boldItalic.fauxItalic = false;
  } else if (haveItalic) {
    boldItalic.sourceIndex = ITALIC;
    boldItalic.fauxBold = true;
    boldItalic.fauxItalic = false;
  } else if (haveBold) {
    boldItalic.sourceIndex = BOLD;
    boldItalic.fauxBold = false;
    boldItalic.fauxItalic = true;
  } else {
    boldItalic.sourceIndex = REGULAR;
    boldItalic.fauxBold = true;
    boldItalic.fauxItalic = true;
  }
}

void TtfEpdFont::Impl::setupFace(Face& face) {
  const stbtt_fontinfo& info = sources[face.sourceIndex].info;
  // Points at 150 DPI map the em square to pixelsPerEm, as FreeType's
  // FT_Set_Char_Size does for the .cpfont converter and the built-in fonts.
  face.scale = stbtt_ScaleForMappingEmToPixels(&info, pixelsPerEm);

  int ascent = 0;
  int descent = 0;
  int lineGap = 0;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
  const int lineHeight = static_cast<int>(ceilf(static_cast<float>(ascent - descent + lineGap) * face.scale));

  face.data = EpdFontData{};
  face.data.advanceY = static_cast<uint8_t>(std::clamp(lineHeight, 1, 255));
  face.data.ascender = static_cast<int>(ceilf(static_cast<float>(ascent) * face.scale));
  face.data.descender = static_cast<int>(floorf(static_cast<float>(descent) * face.scale));  // negative, as built-ins
  face.data.is2Bit = true;
  face.data.glyphMissHandler = &Impl::missThunk;
  face.data.glyphMissCtx = &face;
  face.data.kernHandler = &Impl::kernThunk;
  face.data.bitmap = face.arena;

  // No GSUB here: the standard Latin ligatures are offered when the font maps
  // their presentation codepoints, which is where EpdFont::applyLigatures()
  // sends them anyway. Entries are appended in ascending key order, as its
  // binary search requires ('f' pairs sort below the U+FB00-chained ones).
  const auto has = [&info](const uint32_t codepoint) { return stbtt_FindGlyphIndex(&info, codepoint) != 0; };
  uint8_t pairCount = 0;
  const auto add = [&](const uint32_t left, const uint32_t right, const uint32_t ligature) {
    face.ligaturePairs[pairCount++] = EpdLigaturePair{(left << 16) | right, ligature};
  };
  const bool hasFf = has(0xFB00);
  if (hasFf) add('f', 'f', 0xFB00);
  if (has(0xFB01)) add('f', 'i', 0xFB01);
  if (has(0xFB02)) add('f', 'l', 0xFB02);
  if (hasFf && has(0xFB03)) add(0xFB00, 'i', 0xFB03);
  if (hasFf && has(0xFB04)) add(0xFB00, 'l', 0xFB04);
  face.data.ligaturePairs = pairCount ? face.ligaturePairs : nullptr;
  face.data.ligaturePairCount = pairCount;
}

TtfEpdFont::LoadResult TtfEpdFont::load(const uint8_t pointSize) {
  impl_->loaded = false;
  if (!impl_->sources[REGULAR].parsed || pointSize == 0) return LoadResult::INVALID_FONT;

  releaseCaches();  // glyphs rasterised at a previous size must not survive
  impl_->pixelsPerEm = static_cast<float>(pointSize) * 150.0f / 72.0f;
  // Synthesised bold widens every stroke by this many pixels.
  impl_->boldPixels = static_cast<uint8_t>(std::clamp(static_cast<int>(lroundf(impl_->pixelsPerEm / 24.0f)), 1, 4));
  impl_->resolveFaces();
  for (auto& face : impl_->faces) impl_->setupFace(face);
  // The regular style's cache is allocated now rather than on its first glyph,
  // so a font that cannot get one is refused here, before any layout uses it.
  if (!impl_->ensureCache(impl_->faces[REGULAR])) return LoadResult::UNAVAILABLE;
  impl_->loaded = true;
  return LoadResult::OK;
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(&impl_->faces[REGULAR].font, &impl_->faces[BOLD].font, &impl_->faces[ITALIC].font,
                       &impl_->faces[BOLD_ITALIC].font);
}

uint32_t TtfEpdFont::contentHash() const {
  const uint8_t revision[4] = {
      static_cast<uint8_t>(TTF_RENDER_REVISION), static_cast<uint8_t>(TTF_RENDER_REVISION >> 8),
      static_cast<uint8_t>(TTF_RENDER_REVISION >> 16), static_cast<uint8_t>(TTF_RENDER_REVISION >> 24)};
  return fnv1a(impl_->contentHash, revision, sizeof(revision));
}

bool TtfEpdFont::Impl::ensureCache(Face& face) {
  if (face.cacheBlock) return true;
  if (face.cacheAllocFailed) return false;  // retried after the next releaseCaches()/load()

  // Section order keeps every table naturally aligned: 4-byte tables first,
  // then the 2-byte slots, then the byte arena.
  const size_t glyphBytes = sizeof(EpdGlyph) * MAX_GLYPHS;
  const size_t codepointBytes = sizeof(uint32_t) * MAX_GLYPHS;
  const size_t kernBytes = sizeof(KernEntry) * KERN_SLOTS;
  const size_t slotBytes = sizeof(uint16_t) * MAX_GLYPHS;
  const size_t total = glyphBytes + codepointBytes + kernBytes + slotBytes + ARENA_BYTES;
  static_assert(alignof(EpdGlyph) <= 4 && alignof(KernEntry) <= 4, "cache block layout assumes 4-byte alignment");

  // No PSRAM_RESERVE check here, unlike the font files: once a font is loaded,
  // a refused cache means text that cannot be measured, which is worse for the
  // reader than dipping into the reserve.
  if (!scratch) scratch = static_cast<uint8_t*>(ttfAlloc(SCRATCH_BYTES));
  auto* block = scratch ? static_cast<uint8_t*>(ttfAlloc(total)) : nullptr;
  if (!block) {
    LOG_ERR("TTF", "PSRAM alloc failed: glyph cache (%u bytes)", static_cast<unsigned>(total));
    face.cacheAllocFailed = true;
    return false;
  }
  face.cacheBlock = block;
  face.glyphs = reinterpret_cast<EpdGlyph*>(block);
  face.codepoints = reinterpret_cast<uint32_t*>(block + glyphBytes);
  face.kern = reinterpret_cast<KernEntry*>(block + glyphBytes + codepointBytes);
  face.slots = reinterpret_cast<uint16_t*>(block + glyphBytes + codepointBytes + kernBytes);
  face.arena = block + glyphBytes + codepointBytes + kernBytes + slotBytes;
  memset(face.kern, 0, kernBytes);
  face.glyphCount = 0;
  face.arenaUsed = 0;
  face.data.bitmap = face.arena;
  return true;
}

void TtfEpdFont::Impl::flushGlyphs(Face& face) {
  face.glyphCount = 0;
  face.arenaUsed = 0;
}

void TtfEpdFont::Impl::releaseFaceCache(Face& face) {
  ttfFree(face.cacheBlock);
  face.cacheBlock = nullptr;
  face.cacheAllocFailed = false;
  face.glyphs = nullptr;
  face.codepoints = nullptr;
  face.slots = nullptr;
  face.kern = nullptr;
  face.arena = nullptr;
  face.arenaUsed = 0;
  face.glyphCount = 0;
  face.data.bitmap = nullptr;
}

void TtfEpdFont::releaseCaches() {
  for (auto& face : impl_->faces) impl_->releaseFaceCache(face);
  ttfFree(impl_->scratch);
  impl_->scratch = nullptr;
}

void TtfEpdFont::releaseAllCaches() {
  for (TtfEpdFont* font = liveHead_; font; font = font->nextLive_) font->releaseCaches();
}

bool TtfEpdFont::Impl::rasterise(Face& face, const int glyphIndex, EpdGlyph& out) {
  const stbtt_fontinfo& info = sources[face.sourceIndex].info;

  int advanceUnits = 0;
  int leftSideBearing = 0;
  stbtt_GetGlyphHMetrics(&info, glyphIndex, &advanceUnits, &leftSideBearing);
  int32_t advance = static_cast<int32_t>(lroundf(static_cast<float>(advanceUnits) * face.scale * 16.0f));
  // Zero-advance glyphs (combining marks) must stay zero-advance.
  if (face.fauxBold && advance > 0) advance += fp4::fromPixel(boldPixels);
  out = EpdGlyph{};
  out.advanceX = static_cast<uint16_t>(std::clamp<int32_t>(advance, 0, 0xFFFF));
  out.dataOffset = face.arenaUsed;

  stbtt_vertex* vertices = nullptr;
  const int vertexCount = stbtt_GetGlyphShape(&info, glyphIndex, &vertices);
  if (vertexCount <= 0 || !vertices) {
    if (vertices) stbtt_FreeShape(&info, vertices);
    return true;  // no outline (space): metrics only
  }

  // Bounding box in font units over on-curve and control points (a curve lies
  // inside the hull of its control points), applying the italic shear first.
  int minX = INT32_MAX;
  int minY = INT32_MAX;
  int maxX = INT32_MIN;
  int maxY = INT32_MIN;
  const auto shearX = [&face](const int16_t x, const int16_t y) -> int16_t {
    return face.fauxItalic ? clampToShort(static_cast<float>(x) + ITALIC_SHEAR * static_cast<float>(y)) : x;
  };
  const auto extend = [&](const int x, const int y) {
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  };
  for (int i = 0; i < vertexCount; i++) {
    stbtt_vertex& vertex = vertices[i];
    vertex.x = shearX(vertex.x, vertex.y);
    extend(vertex.x, vertex.y);
    if (vertex.type == STBTT_vcurve || vertex.type == STBTT_vcubic) {
      vertex.cx = shearX(vertex.cx, vertex.cy);
      extend(vertex.cx, vertex.cy);
    }
    if (vertex.type == STBTT_vcubic) {
      vertex.cx1 = shearX(vertex.cx1, vertex.cy1);
      extend(vertex.cx1, vertex.cy1);
    }
  }

  // Pixel box, y flipped to screen direction (stbtt_GetGlyphBitmapBox's rule).
  const int left = static_cast<int>(floorf(static_cast<float>(minX) * face.scale));
  const int right = static_cast<int>(ceilf(static_cast<float>(maxX) * face.scale));
  const int top = static_cast<int>(floorf(static_cast<float>(-maxY) * face.scale));
  const int bottom = static_cast<int>(ceilf(static_cast<float>(-minY) * face.scale));
  const int extraWidth = face.fauxBold ? boldPixels : 0;
  const int width = right - left + extraWidth;
  const int height = bottom - top;
  if (right <= left || height <= 0) {
    stbtt_FreeShape(&info, vertices);
    return true;
  }
  if (width > MAX_GLYPH_DIMENSION || height > MAX_GLYPH_DIMENSION) {
    if (!oversizeLogged) {
      LOG_ERR("TTF", "Glyph %d is %dx%d px, over the %d px glyph limit; drawn blank", glyphIndex, width, height,
              MAX_GLYPH_DIMENSION);
      oversizeLogged = true;
    }
    stbtt_FreeShape(&info, vertices);
    return true;
  }

  const uint32_t pixelCount = static_cast<uint32_t>(width) * static_cast<uint32_t>(height);
  const uint32_t packedBytes = (pixelCount + 3) / 4;
  if (face.arenaUsed + packedBytes > ARENA_BYTES) {
    stbtt_FreeShape(&info, vertices);
    return false;  // caller flushes and retries
  }

  memset(scratch, 0, pixelCount);
  stbtt__bitmap coverage;
  coverage.w = width;
  coverage.h = height;
  coverage.stride = width;
  coverage.pixels = scratch;
  stbtt_Rasterize(&coverage, 0.35f, vertices, vertexCount, face.scale, face.scale, 0.0f, 0.0f, left, top, 1, nullptr);
  stbtt_FreeShape(&info, vertices);

  if (extraWidth > 0) {
    // Synthesised bold: smear coverage to the right. Walking each row right to
    // left reads only pixels this pass has not rewritten yet.
    for (int y = 0; y < height; y++) {
      uint8_t* row = scratch + y * width;
      for (int x = width - 1; x >= 0; x--) {
        uint8_t value = row[x];
        for (int step = 1; step <= extraWidth && x - step >= 0; step++) value = std::max(value, row[x - step]);
        row[x] = value;
      }
    }
  }

  // Pack to the renderer's 2-bit format (MSB first, rows bit-contiguous) with
  // the .cpfont converter's 25/50/75% thresholds, so a vector font prints at
  // the stroke weight of the same face converted offline.
  uint8_t* packed = face.arena + face.arenaUsed;
  memset(packed, 0, packedBytes);
  for (uint32_t i = 0; i < pixelCount; i++) {
    const uint8_t alpha = scratch[i];
    const uint8_t level = alpha < 64 ? 0 : alpha < 128 ? 1 : alpha < 192 ? 2 : 3;
    packed[i >> 2] |= static_cast<uint8_t>(level << ((3 - (i & 3)) * 2));
  }

  out.width = static_cast<uint8_t>(width);
  out.height = static_cast<uint8_t>(height);
  out.left = static_cast<int16_t>(left);
  out.top = static_cast<int16_t>(-top);  // EpdGlyph::top is positive above the baseline
  out.dataLength = static_cast<uint16_t>(std::min<uint32_t>(packedBytes, 0xFFFF));
  face.arenaUsed += packedBytes;
  return true;
}

const EpdGlyph* TtfEpdFont::Impl::faultGlyph(Face& face, const uint32_t codepoint) {
  if (!loaded) return nullptr;
  if (!ensureCache(face)) {
    // No PSRAM for this style's cache: draw the style with the regular face
    // rather than return nothing. A missing glyph has no advance, and a section
    // laid out with zero-width words would be written to the section cache.
    Face& regular = faces[REGULAR];
    if (&face == &regular || !ensureCache(regular)) return nullptr;
    face.data.bitmap = regular.arena;  // the glyph's dataOffset indexes the regular arena
    return faultGlyph(regular, codepoint);
  }

  uint32_t* end = face.codepoints + face.glyphCount;
  uint32_t* position = std::lower_bound(face.codepoints, end, codepoint);
  if (position != end && *position == codepoint) return &face.glyphs[face.slots[position - face.codepoints]];

  const stbtt_fontinfo& info = sources[face.sourceIndex].info;
  const int glyphIndex = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
  // An unmapped codepoint returns nullptr so EpdFont::getGlyph() retries with
  // U+FFFD; a font without U+FFFD answers that with .notdef (glyph 0), its own
  // missing-glyph box, rather than leaving every such character unreported.
  if (glyphIndex == 0 && codepoint != REPLACEMENT_GLYPH) return nullptr;

  EpdGlyph glyph;
  bool placed = face.glyphCount < MAX_GLYPHS && rasterise(face, glyphIndex, glyph);
  if (!placed) {
    // Table or arena full: drop this style's glyphs and start over. Callers
    // consume a glyph before asking for the next (EpdFontData contract), so no
    // pointer handed out earlier is still in use.
    flushGlyphs(face);
    placed = rasterise(face, glyphIndex, glyph);
    if (!placed) return nullptr;
  }

  end = face.codepoints + face.glyphCount;
  position = std::lower_bound(face.codepoints, end, codepoint);
  const size_t index = static_cast<size_t>(position - face.codepoints);
  const size_t tail = face.glyphCount - index;
  memmove(position + 1, position, tail * sizeof(uint32_t));
  memmove(face.slots + index + 1, face.slots + index, tail * sizeof(uint16_t));
  face.codepoints[index] = codepoint;
  face.slots[index] = face.glyphCount;
  face.glyphs[face.glyphCount] = glyph;
  return &face.glyphs[face.glyphCount++];
}

int8_t TtfEpdFont::Impl::kerning(Face& face, const uint32_t leftCp, const uint32_t rightCp) {
  if (!loaded || leftCp == 0 || rightCp == 0) return 0;
  const Source& source = sources[face.sourceIndex];
  if (!source.hasKerning) return 0;

  // getKerning() runs for every adjacent pair on every measure and draw pass;
  // the table walk behind it is worth a direct-mapped cache.
  KernEntry* entry = nullptr;
  if (ensureCache(face)) {
    entry = &face.kern[((leftCp * 31u) ^ (rightCp * 131u)) & (KERN_SLOTS - 1)];
    if (entry->left == leftCp && entry->right == rightCp) return entry->value;
  }

  int value = 0;
  const int leftGlyph = stbtt_FindGlyphIndex(&source.info, static_cast<int>(leftCp));
  const int rightGlyph = stbtt_FindGlyphIndex(&source.info, static_cast<int>(rightCp));
  if (leftGlyph != 0 && rightGlyph != 0) {
    const int units = stbtt_GetGlyphKernAdvance(&source.info, leftGlyph, rightGlyph);
    // Font units to 4.4 fixed-point pixels, clamped to the int8 contract.
    value = std::clamp(static_cast<int>(lroundf(static_cast<float>(units) * face.scale * 16.0f)), -128, 127);
  }
  if (entry) *entry = KernEntry{leftCp, rightCp, static_cast<int8_t>(value)};
  return static_cast<int8_t>(value);
}

const EpdGlyph* TtfEpdFont::Impl::missThunk(void* ctx, const uint32_t codepoint) {
  auto* face = static_cast<Face*>(ctx);
  return face->owner->faultGlyph(*face, codepoint);
}

int8_t TtfEpdFont::Impl::kernThunk(void* ctx, const uint32_t leftCp, const uint32_t rightCp) {
  auto* face = static_cast<Face*>(ctx);
  return face->owner->kerning(*face, leftCp, rightCp);
}

#endif  // CROSSPOINT_VECTOR_FONTS
