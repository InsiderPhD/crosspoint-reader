#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <TtfEpdFont.h>

#include <cstring>

#include "CrossPointSettings.h"

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

#if CROSSPOINT_VECTOR_FONTS
namespace {

// A vector font renders at any size; the four font-size steps use the built-in
// reader families' point sizes, so switching to or from a TrueType font keeps
// the text at the same scale (points at 150 DPI, like every other font here).
constexpr uint8_t VECTOR_READER_POINT_SIZES[CrossPointSettings::FONT_SIZE_COUNT] = {6, 8, 10, 12};

// Renderer font id for a vector family at a size: FNV-1a continued from the
// font files' content hash. The section cache stores this id as its font
// identity, so a different family, size or file content never reuses a layout.
int computeTtfFontId(const uint32_t contentHash, const char* familyName, const uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash ^ 0x54544600u;  // "TTF\0": keeps clear of the .cpfont id space
  for (const char* p = familyName; *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is the "not found" sentinel
}

}  // namespace
#endif  // CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family && family->isVector()) {
      // A vector family is loaded by the first ensureLoaded() (entering the
      // reader or the font preview), not here: reading megabytes of font would
      // slow every boot, and a font file that crashes the rasteriser would
      // otherwise do so before the user can reach Settings to deselect it.
      LOG_DBG("SDFS", "Vector font family %s selected; loading on first use", SETTINGS.sdFontFamilyName);
    } else if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;

#if CROSSPOINT_VECTOR_FONTS
  // A vector (.ttf/.otf) family goes through TtfEpdFont, and replaces any
  // .cpfont family that was loaded.
  if (wantedFamily[0] != '\0') {
    const auto* vectorFamily = registry_.findFamily(wantedFamily);
    if (vectorFamily && vectorFamily->isVector()) {
      if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
      loadTtfFamily(*vectorFamily, renderer, registryWasDirty);
      return;
    }
  }
  // Not on a vector family: give its PSRAM back before the paths below run.
  unloadTtf(renderer);
#endif

  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    auto sizes = family->availableSizes();
    uint8_t idx = sizeEnum;
    if (idx >= sizes.size()) idx = sizes.size() - 1;
    uint8_t wantedPt = sizes.empty() ? 0 : sizes[idx];
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
#if CROSSPOINT_VECTOR_FONTS
  // A loaded vector family answers first; the .cpfont manager never holds it.
  if (ttfFontId_ != 0 && familyName && strcmp(ttfFamily_, familyName) == 0) return ttfFontId_;
#endif
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

#if CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  if (!ttf_) return;
  if (ttfFontId_ != 0) renderer.removeFont(ttfFontId_);  // before the faces it points at go away
  TtfEpdFont::destroy(ttf_);
  ttf_ = nullptr;
  ttfFamily_[0] = '\0';
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool forceReload) {
  const uint8_t pointSize = VECTOR_READER_POINT_SIZES[fontSizeEnumFromSettings()];
  const bool sameFamily = ttf_ && family.name == ttfFamily_;
  if (sameFamily && !forceReload && pointSize == ttfPointSize_) return;

  if (sameFamily && !forceReload) {
    // Only the size changed (font size setting, live preview): the font files
    // are already resident, so rebuild the faces without touching the card.
    renderer.removeFont(ttfFontId_);
    ttfFontId_ = 0;
    if (ttf_->load(pointSize) == TtfEpdFont::LoadResult::OK) {
      const int fontId = computeTtfFontId(ttf_->contentHash(), ttfFamily_, pointSize);
      if (renderer.getFontMap().count(fontId) == 0) {
        renderer.insertFont(fontId, ttf_->family());
        ttfFontId_ = fontId;
        ttfPointSize_ = pointSize;
        LOG_DBG("SDFS", "Vector font %s resized to %upt (id=%d)", ttfFamily_, pointSize, fontId);
        return;
      }
    }
    // Fall through to a clean reload.
  }

  unloadTtf(renderer);

  ttf_ = TtfEpdFont::create();
  if (!ttf_) return;  // out of PSRAM: keep the selection, the reader falls back to the built-in family

  // files[] holds one entry per style role (registry): 0=regular is required,
  // the others replace a synthesised style with the real design.
  TtfEpdFont::LoadResult result = TtfEpdFont::LoadResult::INVALID_FONT;
  for (const auto& file : family.files) {
    if (file.style == TtfEpdFont::REGULAR) result = ttf_->addSource(TtfEpdFont::REGULAR, file.path.c_str());
  }
  if (result == TtfEpdFont::LoadResult::OK) {
    for (const auto& file : family.files) {
      if (file.style != TtfEpdFont::REGULAR && file.style < TtfEpdFont::STYLE_COUNT) {
        if (ttf_->addSource(file.style, file.path.c_str()) != TtfEpdFont::LoadResult::OK) {
          LOG_ERR("SDFS", "%s: style file %s not loaded; that style will be synthesised", family.name.c_str(),
                  file.path.c_str());
        }
      }
    }
    result = ttf_->load(pointSize);
  }

  if (result != TtfEpdFont::LoadResult::OK) {
    TtfEpdFont::destroy(ttf_);
    ttf_ = nullptr;
    if (result == TtfEpdFont::LoadResult::INVALID_FONT) {
      // Deterministic: this file will never load. Deselect it, as a .cpfont
      // family that fails to load is deselected.
      LOG_ERR("SDFS", "Vector font family %s is not usable (clearing)", family.name.c_str());
      SETTINGS.sdFontFamilyName[0] = '\0';
    } else {
      // Out of PSRAM or an SD read error: keep the selection so the next
      // ensureLoaded() retries. Until then resolveFontId() returns 0 and the
      // reader uses the built-in family.
      LOG_ERR("SDFS", "Vector font family %s could not be loaded now (keeping selection)", family.name.c_str());
    }
    return;
  }

  const int fontId = computeTtfFontId(ttf_->contentHash(), family.name.c_str(), pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDFS", "Font ID %d collides with an existing font, not loading %s", fontId, family.name.c_str());
    TtfEpdFont::destroy(ttf_);
    ttf_ = nullptr;
    return;
  }
  renderer.insertFont(fontId, ttf_->family());
  snprintf(ttfFamily_, sizeof(ttfFamily_), "%s", family.name.c_str());
  ttfFontId_ = fontId;
  ttfPointSize_ = pointSize;
  LOG_DBG("SDFS", "Loaded vector font family %s at %upt (id=%d, %u style file(s))", ttfFamily_, pointSize, fontId,
          static_cast<unsigned>(family.files.size()));
}

#endif  // CROSSPOINT_VECTOR_FONTS
