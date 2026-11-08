#pragma once
#include <functional>
#include <string>

#include "../../fontIds.h"
#include "../Activity.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  int startY = 0;
  int lineHeight = 0;

#if FREEINK_CAP_TOUCH
  // The X4 Pro has no bottom hint bar (buttonHintsHeight = 0), so the
  // Cancel/Confirm labels are drawn as real on-screen buttons instead, and
  // taps hit-test them. Geometry is deterministic from startY, shared by
  // render() and loop().
  static constexpr int BTN_W = 150;
  static constexpr int BTN_H = 44;
  static constexpr int BTN_GAP = 40;
  int buttonsY() const;
  void finishWith(bool cancelled);
#endif

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // loop() hit-tests its own Cancel/Confirm buttons (X4 Pro). Without this the
  // main loop also turned the tap into an injected Confirm press whose release
  // landed on the screen beneath once the dialog had closed: confirming a
  // delete in the File Browser then opened whichever book the cursor was on.
  bool handlesDirectTouch() const override { return true; }
};