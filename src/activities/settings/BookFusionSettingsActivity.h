#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Settings submenu for BookFusion Sync.
 * Account-only: "Link Account" and "Unlink Account" with linked/not-linked
 * status. Library browsing lives under the Network-mode menu, not here.
 */
class BookFusionSettingsActivity final : public Activity {
 public:
  explicit BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int MENU_ITEMS = 2;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
};
