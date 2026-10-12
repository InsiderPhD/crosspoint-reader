#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;
class TtfEpdFont;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
#if CROSSPOINT_VECTOR_FONTS
  // --- TrueType/OpenType families (TtfEpdFont; PSRAM boards only) ------------
  // Load the selected vector family at the size the font-size setting maps to
  // and register it with the renderer as an ordinary font family. A no-op when
  // that family and size are already loaded, unless forceReload.
  void loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, bool forceReload);
  // Unregister and free the loaded vector family, if any.
  void unloadTtf(GfxRenderer& renderer);

  TtfEpdFont* ttf_ = nullptr;  // owned; PSRAM (TtfEpdFont::create)
  char ttfFamily_[32] = "";    // loaded vector family ("" = none); sized as sdFontFamilyName
  int ttfFontId_ = 0;          // renderer font id for ttf_ (0 = none)
  uint8_t ttfPointSize_ = 0;   // size ttf_ was built at
#endif

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};
