#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Settings submenu for BookFusion Sync.
 * "Link Account" and "Unlink Account" with linked/not-linked status, the
 * download destination (folder, file name), the push extras (highlights,
 * reading time) and the Refresh BookFusion Data action. Library browsing lives
 * under the Network-mode menu, not here. The parent Settings screen saves
 * settings when this submenu returns.
 */
class BookFusionSettingsActivity final : public Activity {
 public:
  explicit BookFusionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BookFusionSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

  static constexpr int MENU_ITEMS = 7;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  void handleSelection();
  Rect listRect() const;
};
