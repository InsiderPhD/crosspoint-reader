#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Picks the compiled sleep theme: one row per folder under /sleep-themes that
 * holds a theme.bin (plus the built-in themes), then a last row that
 * opens the theme store (SleepThemeDownloadActivity, which also updates
 * installed themes).
 *
 * Confirm on a theme offers Use or Delete.
 *
 * Themes come from the store or from the web UI's Sleep Themes page. With none
 * installed, or the configured one missing, the screen says so above the list.
 */
class SleepThemeSelectActivity final : public Activity {
 public:
  explicit SleepThemeSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SleepThemeSelect", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  void discover();
  void addBuiltIns();
  void useTheme(const std::string& name);
  void handleSelection();
  void openThemeActions(int index);
  void confirmDelete(const std::string& name);
  void deleteTheme(const std::string& name);
  void openStore();
  // Points the cursor at the configured theme, or at the store row when that
  // theme isn't installed (so the way to get it is one Confirm away).
  void placeCursor();
  Rect listRect() const;
  // Themes, then the store row.
  int totalItems() const { return static_cast<int>(themes.size()) + 1; }
  int storeIndex() const { return totalItems() - 1; }

  ButtonNavigator buttonNavigator;
  // Rescanned on entry so themes installed since the last visit show up.
  // Always holds the built-in themes (SleepThemeRenderer::BUILT_IN_THEMES).
  std::vector<std::string> themes;
  // SETTINGS.sleepTheme names a folder that isn't here: the screen explains
  // and leads to the store instead of silently listing other themes.
  bool configuredMissing = false;
  int selectedIndex = 0;
};
