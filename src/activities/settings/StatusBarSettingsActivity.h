#pragma once
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Reader status bar configuration. One class serves three screens: the hub
// (Customise Status Bar: an entry per bar plus the settings both bars share)
// and one page per bar, where every element gets its own Hide / Left / Center
// / Right picker and the bar's fill bar is configured, with that bar previewed
// live. The two bars are configured independently, so the same element can
// sit on both.
class StatusBarSettingsActivity final : public Activity {
 public:
  enum class Page : uint8_t { Hub, BottomBar, TopBar };

  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const Page page = Page::Hub)
      : Activity("StatusBarSettings", renderer, mappedInput), page(page) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }

 private:
  const Page page;
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  // Decided in onEnter() based on halClock.isAvailable() so clock entries are hidden on X4.
  int visibleItemCount = 0;

  bool isTopBar() const { return page == Page::TopBar; }
  void handleSelection();
  std::string valueText(int index) const;
  Rect listRect() const;
  int topPreviewY() const;
  int bottomPreviewLabelY() const;
};
