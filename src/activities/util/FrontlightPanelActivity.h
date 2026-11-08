#pragma once

#include <HalFrontlight.h>

#if FREEINK_CAP_FRONTLIGHT

#include <GfxRenderer.h>

#include <cstdint>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/**
 * Light panel: a card hanging from the top of the screen with a brightness and a
 * warmth slider, -/+ steppers, an on/off button and a dark mode switch. Mirrors the layout of upstream
 * CrossPoint's control center so people moving between firmwares find the light
 * where they expect it, but drawn with our own renderer/GUI calls and our
 * brightness model (0 = off, frontlightLastBrightness = the level to come back to).
 *
 * Drawn over whatever the framebuffer already holds (the page or screen beneath),
 * so it never clears the screen; the activity underneath repaints on pop.
 *
 * Opened by READER_ACTION_LIGHT_PANEL and by a tap on the status bar of Home,
 * the file browser, the library and Settings. Never by a swipe.
 *
 * The light follows every change live; settings are written once, on exit, and
 * only if something changed.
 */
class FrontlightPanelActivity final : public Activity {
 public:
  FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FrontlightPanel", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handlesDirectTouch() const override { return true; }
  // Only while a slider is being dragged: the drag would otherwise end as a
  // swipe and inject Back. The rest of the time the Back swipe closes the panel.
  bool ownsSwipes() const override { return dragRow >= 0; }
  bool handlesHomeTap() const override { return true; }
  // Opened over the reader, the panel must not cost a connected page turner.
  bool keepsBluetoothActive() const override;

 private:
  enum Row : int8_t { ROW_BRIGHTNESS = 0, ROW_WARMTH = 1 };

  // One slider row's geometry, laid out once in onEnter() (orientation cannot
  // change while the panel is up) and shared by render() and hit-testing.
  struct RowLayout {
    int captionY = 0;
    Rect minus;
    Rect track;
    Rect plus;
    Rect toggle;  // width 0 on rows without one
  };

  void layout();
  // Slider rows; the dark mode row comes after them, at index sliderCount().
  int sliderCount() const { return hasWarmthRow ? 2 : 1; }
  int darkRow() const { return sliderCount(); }
  int rowCount() const { return sliderCount() + 1; }
  uint8_t rowValue(int row) const { return row == ROW_BRIGHTNESS ? brightness : warmth; }
  void setRowValue(int row, int value);
  int valueAtX(int row, int x) const;
  void applyLight() const;
  void toggleLight();
  void toggleDarkMode();
  void close();
  void drawRow(int row) const;
  void drawDarkRow() const;

  ButtonNavigator buttonNavigator;
  RowLayout rows[2];  // filled by layout()
  Rect darkToggle;
  int darkCaptionY = 0;
  int panelBottom = 0;
  bool hasWarmthRow = false;

  uint8_t brightness = 0;  // 1-100: the level the light is (or will be) on at
  uint8_t warmth = 0;
  bool lightOn = false;
  bool dirty = false;
  // Dark mode flipped since the last render: the screen beneath the card is
  // still in the old polarity and gets inverted on the next render.
  bool invertBeneath = false;

  int focusedRow = ROW_BRIGHTNESS;  // button navigation only
  int dragRow = -1;
};

#endif  // FREEINK_CAP_FRONTLIGHT
