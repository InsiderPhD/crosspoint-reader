#include "SdCardFontManager.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <cstdlib>

#if __has_include(<DevicePolicy.h>)
#include <DevicePolicy.h>
#endif
#ifndef CROSSPOINT_EPUB_BLOCK_FONT_SIZES
#define CROSSPOINT_EPUB_BLOCK_FONT_SIZES 0
#endif
#ifndef CROSSPOINT_TIGHT_HEAP
#define CROSSPOINT_TIGHT_HEAP 1
#endif

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

int SdCardFontManager::loadFile(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file,
                                GfxRenderer& renderer) {
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  if (!family.filePath(file, path, sizeof(path))) {
    LOG_ERR("SDMGR", "No loadable file for size %u in family %s", file.pointSize, family.name.c_str());
    return 0;
  }

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", path);
    return 0;
  }

  if (!font->load(path)) {
    LOG_ERR("SDMGR", "Failed to load %s", path);
    delete font;
    return 0;
  }

  const int fontId = computeFontId(font->contentHash(), family.name.c_str(), file.pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, path);
    delete font;
    return 0;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, file.pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", path, file.pointSize, fontId, font->styleCount());

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily.withPointSize(file.pointSize));
  return fontId;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum) {
  // Unload any previously loaded family first
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  // Select by ordinal position: sort available sizes, then map the font size
  // enum (SMALL=0 .. EXTRA_LARGE=3) to the corresponding slot. When the
  // family has fewer sizes than 4, clamp to the last available size.
  auto sizes = family.availableSizes();
  if (sizes.empty()) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  uint8_t idx = fontSizeEnum;
  if (idx >= sizes.size()) idx = sizes.size() - 1;
  const SdCardFontFileInfo* selected = family.findFile(sizes[idx]);
  // Body plus at most three extra sizes: one allocation for the owner list.
  loaded_.reserve(4);
  if (!selected || loadFile(family, *selected, renderer) == 0) return false;

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
#if CROSSPOINT_EPUB_BLOCK_FONT_SIZES
  loadExtraSizes(family, renderer);
#endif
  return true;
}

void SdCardFontManager::loadExtraSizes(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  // The sizes headings and CSS font-size ask for most: h1 (2x) and h2 (1.5x); PSRAM boards
  // also take a smaller one (3/4) for small print. Each is the family's nearest file.
  const unsigned body = loadedPointSize_;
#if CROSSPOINT_TIGHT_HEAP
  const unsigned targets[] = {body * 2, body * 3 / 2};
  // C3: an extra size keeps its interval / kern tables resident next to the body font, and
  // fills a glyph cache when a page uses it. Only with a healthy heap (CrossInk's reserve),
  // and only when the font's resident cost stays inside a small budget; otherwise it is
  // unloaded again and headings keep the body size.
  static constexpr uint32_t EXTRA_MIN_FREE_HEAP = 108 * 1024;
  static constexpr uint32_t EXTRA_MIN_MAX_ALLOC = 60 * 1024;
  static constexpr uint32_t EXTRA_RESIDENT_BUDGET = 12 * 1024;
#else
  const unsigned targets[] = {body * 2, body * 3 / 2, body * 3 / 4};
#endif
  for (const unsigned target : targets) {
    const SdCardFontFileInfo* best = nullptr;
    for (const auto& file : family.files) {
      if (!best || std::abs(static_cast<int>(file.pointSize) - static_cast<int>(target)) <
                       std::abs(static_cast<int>(best->pointSize) - static_cast<int>(target))) {
        best = &file;
      }
    }
    if (!best) continue;
    bool present = false;
    for (const auto& lf : loaded_) present = present || lf.size == best->pointSize;
    if (present) continue;
#if CROSSPOINT_TIGHT_HEAP
    const uint32_t freeBefore = ESP.getFreeHeap();
    if (freeBefore < EXTRA_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < EXTRA_MIN_MAX_ALLOC) {
      LOG_DBG("SDMGR", "Skipping extra size %u: heap reserve (%u free)", best->pointSize,
              static_cast<unsigned>(freeBefore));
      continue;
    }
    const int fontId = loadFile(family, *best, renderer);
    const uint32_t freeAfter = ESP.getFreeHeap();
    if (fontId != 0 && freeBefore > freeAfter && freeBefore - freeAfter > EXTRA_RESIDENT_BUDGET) {
      LOG_DBG("SDMGR", "Extra size %u costs %u bytes, over budget: unloaded", best->pointSize,
              static_cast<unsigned>(freeBefore - freeAfter));
      renderer.removeFont(fontId);  // also drops the SD registration
      delete loaded_.back().font;
      loaded_.pop_back();
    }
#else
    loadFile(family, *best, renderer);
#endif
  }
}

void SdCardFontManager::ladder(int* fontIds, uint8_t* points, const size_t slots) const {
  for (size_t i = 0; i < slots; ++i) {
    fontIds[i] = 0;
    points[i] = 0;
  }
  // Insertion by size into the caller's slots (at most 4 loaded fonts).
  size_t count = 0;
  for (const auto& lf : loaded_) {
    if (count == slots) break;
    size_t at = count++;
    while (at > 0 && points[at - 1] > lf.size) {
      points[at] = points[at - 1];
      fontIds[at] = fontIds[at - 1];
      --at;
    }
    points[at] = lf.size;
    fontIds[at] = lf.fontId;
  }
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  return loaded_.front().fontId;
}
