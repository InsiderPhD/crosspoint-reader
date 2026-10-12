#pragma once
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Status bar configuration. One class serves four screens: the hub (Customise
// Status Bar: an entry per bar plus the settings they share) and one page per
// bar -- Outside Reader (the strip along the top of every non-reader screen),
// Reader Top and Reader Bottom -- where every element gets its own Hide / Left
// / Center / Right picker, with that bar previewed live. The bars are
// configured independently, so the same element can sit on all of them.
class StatusBarSettingsActivity final : public Activity {
 public:
  enum class Page : uint8_t { Hub, BottomBar, TopBar, OutsideReader };

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
  // The hub previews both reader bars, each reader page its own; the Outside
  // Reader page needs neither (its own header is the preview).
  bool showsTopPreview() const { return page == Page::Hub || page == Page::TopBar; }
  bool showsBottomPreview() const { return page == Page::Hub || page == Page::BottomBar; }
  void handleSelection();
  std::string valueText(int index) const;
  Rect listRect() const;
  int topPreviewY() const;
  int bottomPreviewLabelY() const;
};
