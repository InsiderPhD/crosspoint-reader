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
 * holds a theme.bin. Choosing one also switches the sleep screen to Theme, since
 * that is the only reason to pick one.
 *
 * Themes are installed from the web UI's Sleep Themes page; with none installed
 * the screen says so instead of showing an empty list.
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
  void handleSelection();
  Rect listRect() const;
  int totalItems() const { return static_cast<int>(themes.size()); }

  ButtonNavigator buttonNavigator;
  // Rescanned on entry so themes installed since the last visit show up.
  std::vector<std::string> themes;
  int selectedIndex = 0;
};
