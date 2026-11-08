#include "FrontlightPanelActivity.h"

#if FREEINK_CAP_FRONTLIGHT

#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FrontlightScheduler.h"
#include "util/FrontlightToggle.h"

namespace {
// Finger-sized control band, the same 56px upstream's panel uses so the two
// look and hit alike.
constexpr int CONTROL_SIZE = 56;
constexpr int CONTROL_GAP = 8;
constexpr int CAPTION_GAP = 8;  // caption baseline band -> control band
constexpr int ROW_GAP = 16;     // after each row
constexpr int BUTTON_RADIUS = 8;
constexpr int OUTLINE = 2;
constexpr int GLYPH_LENGTH = 20;
constexpr int GLYPH_THICKNESS = 3;
constexpr int GRABBER_WIDTH = 40;
constexpr int GRABBER_HEIGHT = 5;
constexpr int GRABBER_MARGIN = 12;  // air above and below the grabber
constexpr int BOTTOM_RULE = 2;
// One percent per step, on the -/+ buttons and the Left/Right keys alike (the
// keys repeat while held), so a level can be set exactly.
constexpr int STEP = 1;
// The slider bottoms out at 1%: off is the On/Off button's job, so a 0% "on"
// level would only be a second way to reach the same place.
constexpr int MIN_BRIGHTNESS = 1;

bool contains(const Rect& r, const int x, const int y) {
  return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}
}  // namespace

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  lightOn = SETTINGS.frontlightBrightness > 0;
  // Off: the slider shows the level On will bring back, as the toggle action does.
  const uint8_t level = lightOn                                 ? SETTINGS.frontlightBrightness
                        : SETTINGS.frontlightLastBrightness > 0 ? SETTINGS.frontlightLastBrightness
                                                                : FrontlightToggle::FALLBACK_BRIGHTNESS;
  brightness = static_cast<uint8_t>(std::clamp<int>(level, MIN_BRIGHTNESS, 100));
  warmth = SETTINGS.frontlightWarmth;
  hasWarmthRow = halFrontlight.hasWarmth();
  dirty = false;
  invertBeneath = false;
  focusedRow = ROW_BRIGHTNESS;
  dragRow = -1;

  layout();
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // One write for the whole visit, however many levels were tried on the way.
  // (dark mode is already in SETTINGS; this save carries it too)
  if (dirty) {
    SETTINGS.frontlightBrightness = lightOn ? brightness : 0;
    if (!lightOn) SETTINGS.frontlightLastBrightness = brightness;
    SETTINGS.frontlightWarmth = warmth;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

bool FrontlightPanelActivity::keepsBluetoothActive() const { return activityManager.readerOnStack(); }

void FrontlightPanelActivity::layout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int left = metrics.contentSidePadding;
  const int right = width - metrics.contentSidePadding;
  const int captionHeight = renderer.getLineHeight(UI_12_FONT_ID);

  // Status-bar band (battery, clock) at the top of the card, as on Home.
  int y = metrics.homeTopPadding;
  for (int row = 0; row < sliderCount(); row++) {
    RowLayout& r = rows[row];
    r.captionY = y;
    const int controlY = y + captionHeight + CAPTION_GAP;
    r.minus = Rect{left, controlY, CONTROL_SIZE, CONTROL_SIZE};
    // Only the brightness row has the On/Off button; warmth's track takes its room.
    const bool withToggle = row == ROW_BRIGHTNESS;
    int trailing = right;
    if (withToggle) {
      r.toggle = Rect{right - CONTROL_SIZE, controlY, CONTROL_SIZE, CONTROL_SIZE};
      trailing = r.toggle.x - CONTROL_GAP;
    } else {
      r.toggle = Rect{};
    }
    r.plus = Rect{trailing - CONTROL_SIZE, controlY, CONTROL_SIZE, CONTROL_SIZE};
    const int trackX = r.minus.x + CONTROL_SIZE + CONTROL_GAP;
    r.track = Rect{trackX, controlY, r.plus.x - CONTROL_GAP - trackX, CONTROL_SIZE};
    y = controlY + CONTROL_SIZE + ROW_GAP;
  }
  // Dark mode: caption on the left, an On/Off button lined up under the light's.
  darkToggle = Rect{right - CONTROL_SIZE, y, CONTROL_SIZE, CONTROL_SIZE};
  darkCaptionY = y + (CONTROL_SIZE - captionHeight) / 2;
  y += CONTROL_SIZE;
  panelBottom = y + GRABBER_MARGIN + GRABBER_HEIGHT + GRABBER_MARGIN + BOTTOM_RULE;
}

void FrontlightPanelActivity::applyLight() const { halFrontlight.apply(lightOn ? brightness : 0, warmth); }

void FrontlightPanelActivity::setRowValue(const int row, int value) {
  if (row == ROW_BRIGHTNESS) {
    value = std::clamp(value, MIN_BRIGHTNESS, 100);
    // Moving the level of a light that is off turns it on, so the slider is
    // never a control that visibly does nothing.
    if (value == brightness && lightOn) return;
    brightness = static_cast<uint8_t>(value);
    lightOn = true;
  } else {
    value = std::clamp(value, 0, 100);
    if (value == warmth) return;
    warmth = static_cast<uint8_t>(value);
  }
  dirty = true;
  applyLight();
  requestUpdate();
}

int FrontlightPanelActivity::valueAtX(const int row, const int x) const {
  // Inverse of drawRow's fill: the round end's centre travels from radius to
  // width - radius, so the value under the finger is where that centre sits.
  const Rect& t = rows[row].track;
  const int travel = std::max(1, t.width - t.height);
  return ((x - t.x - t.height / 2) * 100 + travel / 2) / travel;
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  dirty = true;
  applyLight();
  requestUpdate();
}

void FrontlightPanelActivity::toggleDarkMode() {
  {
    // render() reads and clears invertBeneath on the render task.
    RenderLock lock(*this);
    // Arms the full refresh a polarity flip needs; the panel's own next paint
    // takes it, so the screen beneath flips cleanly in the same refresh.
    FrontlightScheduler::setDarkMode(SETTINGS.darkMode ? CrossPointSettings::DARK_MODE_OFF
                                                       : CrossPointSettings::DARK_MODE_ON);
    invertBeneath = !invertBeneath;  // a second flip before the render cancels out
    dirty = true;
  }
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

void FrontlightPanelActivity::loop() {
#if FREEINK_CAP_TOUCH
  if (mappedInput.wasHomeGesture()) {
    close();
    return;
  }

  int x = 0;
  int y = 0;
  if (dragRow >= 0) {
    // Follow the finger until it lifts. The lift also reports as a tap, which
    // must not land on whatever is under the finger now.
    if (mappedInput.heldTouchPoint(x, y)) {
      setRowValue(dragRow, valueAtX(dragRow, x));
    } else {
      dragRow = -1;
    }
    return;
  }

  if (mappedInput.wasTouchDownPoint(x, y)) {
    for (int row = 0; row < sliderCount(); row++) {
      if (contains(rows[row].track, x, y)) {
        dragRow = row;
        focusedRow = row;
        setRowValue(row, valueAtX(row, x));
        return;
      }
    }
  }

  if (mappedInput.wasTapPoint(x, y)) {
    if (y >= panelBottom) {
      close();
      return;
    }
    if (contains(darkToggle, x, y)) {
      toggleDarkMode();
      return;
    }
    for (int row = 0; row < sliderCount(); row++) {
      const RowLayout& r = rows[row];
      if (contains(r.minus, x, y)) {
        setRowValue(row, rowValue(row) - STEP);
        return;
      }
      if (contains(r.plus, x, y)) {
        setRowValue(row, rowValue(row) + STEP);
        return;
      }
      if (contains(r.toggle, x, y)) {
        toggleLight();
        return;
      }
    }
    return;
  }
#endif

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (focusedRow == darkRow()) {
      toggleDarkMode();
    } else {
      toggleLight();
    }
    return;
  }

  // Up/Down pick the row and Left/Right move its level, so the stock
  // next/previous sets (which pair Down with Right) don't fit here.
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this] {
    focusedRow = ButtonNavigator::nextIndex(focusedRow, rowCount());
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this] {
    focusedRow = ButtonNavigator::previousIndex(focusedRow, rowCount());
    requestUpdate();
  });
  // The dark row has no level: Confirm switches it.
  if (focusedRow == darkRow()) return;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { setRowValue(focusedRow, rowValue(focusedRow) - STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { setRowValue(focusedRow, rowValue(focusedRow) + STEP); });
}

void FrontlightPanelActivity::drawRow(const int row) const {
  const RowLayout& r = rows[row];
  const uint8_t value = rowValue(row);

  // Caption: name on the left, live percentage on the right. The focused row is
  // bold so the Left/Right keys have a visible target; on a touch board there is
  // no focus to show.
  const bool focused = !mappedInput.hasTouch() && row == focusedRow;
  const auto style = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const char* label = row == ROW_BRIGHTNESS ? tr(STR_BRIGHTNESS) : tr(STR_WARMTH);
  renderer.drawText(UI_12_FONT_ID, r.minus.x, r.captionY, label, true, style);
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", static_cast<unsigned>(value));
  const Rect& end = r.toggle.width > 0 ? r.toggle : r.plus;
  const int pctX = end.x + end.width - renderer.getTextWidth(UI_12_FONT_ID, pct, style);
  renderer.drawText(UI_12_FONT_ID, pctX, r.captionY, pct, true, style);

  // -/+ steppers: outlined buttons with a drawn glyph, so no font has to carry them.
  const auto drawStepper = [this](const Rect& b, const bool plus) {
    renderer.drawRoundedRect(b.x, b.y, b.width, b.height, OUTLINE, BUTTON_RADIUS, true);
    const int cx = b.x + b.width / 2;
    const int cy = b.y + b.height / 2;
    renderer.fillRect(cx - GLYPH_LENGTH / 2, cy - GLYPH_THICKNESS / 2, GLYPH_LENGTH, GLYPH_THICKNESS, true);
    if (plus) renderer.fillRect(cx - GLYPH_THICKNESS / 2, cy - GLYPH_LENGTH / 2, GLYPH_THICKNESS, GLYPH_LENGTH, true);
  };
  drawStepper(r.minus, false);
  drawStepper(r.plus, true);

  // Track: an outlined capsule filled from the left up to the value. The fill
  // always keeps its round end (0% is a circle at the left), so a low level
  // never collapses into a sliver. An unlit light greys the brightness fill so
  // the level it will come back on at still reads.
  const Rect& t = r.track;
  const int radius = t.height / 2;
  renderer.drawRoundedRect(t.x, t.y, t.width, t.height, OUTLINE, radius, true);
  const int fillWidth = t.height + (t.width - t.height) * value / 100;
  const Color fill = (row == ROW_BRIGHTNESS && !lightOn) ? Color::LightGray : Color::Black;
  renderer.fillRoundedRect(t.x, t.y, fillWidth, t.height, radius, fill);
  renderer.drawRoundedRect(t.x, t.y, fillWidth, t.height, OUTLINE, radius, true);

  // On/Off: filled while lit, outlined while off.
  if (r.toggle.width > 0) {
    const Rect& b = r.toggle;
    const char* state = lightOn ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    if (lightOn) {
      renderer.fillRoundedRect(b.x, b.y, b.width, b.height, BUTTON_RADIUS, Color::Black);
    } else {
      renderer.drawRoundedRect(b.x, b.y, b.width, b.height, OUTLINE, BUTTON_RADIUS, true);
    }
    const int textX = b.x + (b.width - renderer.getTextWidth(SMALL_FONT_ID, state, EpdFontFamily::BOLD)) / 2;
    const int textY = b.y + (b.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, state, !lightOn, EpdFontFamily::BOLD);
  }
}

void FrontlightPanelActivity::drawDarkRow() const {
  const bool focused = !mappedInput.hasTouch() && focusedRow == darkRow();
  const auto style = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, darkCaptionY, tr(STR_READER_DARK_MODE), true, style);

  // Same filled-while-on button as the light's On/Off.
  const bool on = SETTINGS.darkMode != 0;
  const Rect& b = darkToggle;
  const char* state = on ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
  if (on) {
    renderer.fillRoundedRect(b.x, b.y, b.width, b.height, BUTTON_RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(b.x, b.y, b.width, b.height, OUTLINE, BUTTON_RADIUS, true);
  }
  const int textX = b.x + (b.width - renderer.getTextWidth(SMALL_FONT_ID, state, EpdFontFamily::BOLD)) / 2;
  const int textY = b.y + (b.height - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
  renderer.drawText(SMALL_FONT_ID, textX, textY, state, !on, EpdFontFamily::BOLD);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  // The card. Everything below panelBottom is left as the screen underneath.
  renderer.fillRect(0, 0, width, panelBottom, false);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.homeTopPadding - metrics.topPadding}, nullptr);

  for (int row = 0; row < sliderCount(); row++) drawRow(row);
  drawDarkRow();

  renderer.fillRect(0, panelBottom - BOTTOM_RULE, width, BOTTOM_RULE, true);
  renderer.fillRoundedRect((width - GRABBER_WIDTH) / 2, panelBottom - BOTTOM_RULE - GRABBER_MARGIN - GRABBER_HEIGHT,
                           GRABBER_WIDTH, GRABBER_HEIGHT, GRABBER_HEIGHT / 2, Color::Black);

  // The frame beneath is already in dark-mode polarity, so only the card needs
  // flipping to match it.
  if (SETTINGS.darkMode) renderer.invertRect(0, 0, width, panelBottom);
  // Dark mode was switched from here: bring the page beneath into the new
  // polarity too (the activity underneath repaints it properly on pop).
  if (invertBeneath) {
    renderer.invertRect(0, panelBottom, width, renderer.getScreenHeight() - panelBottom);
    invertBeneath = false;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // FREEINK_CAP_FRONTLIGHT
