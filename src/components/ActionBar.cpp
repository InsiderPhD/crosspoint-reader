#include "ActionBar.h"

#if FREEINK_DEVICE_X4PRO

#include <GfxRenderer.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>

#include <cstring>

namespace {

// Published slot geometry. 4 x 10 bytes of static DRAM rather than a heap
// allocation: the bar has a fixed maximum shape, is republished on every paint,
// and must be readable from the input path without an allocation the touch
// handler could fail.
struct Slot {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint8_t button;  // MappedInputManager::Button
  bool active;
};
Slot slots[4] = {};

// The publish (render task, CPU 1 on this dual-core part) and the hit-test
// (main task, CPU 0) genuinely run in parallel here, so a tap landing mid-paint
// could otherwise read a half-written slot and inject the wrong button. A
// spinlock is the cheap fix; the guarded regions are a few stores each.
portMUX_TYPE slotsMux = portMUX_INITIALIZER_UNLOCKED;

constexpr int SIDE_MARGIN = 12;
constexpr int SLOT_GAP = 10;
// Breathing room ABOVE the buttons only — they run flush to the bottom screen
// edge, which is what lets their bottom corners be square without leaving a
// floating sliver under each one.
constexpr int BAR_TOP_PAD = 4;

bool hasLabel(const char* s) { return s != nullptr && s[0] != '\0'; }

// The outer two slots ARE this board's Left/Right pair -- there is no key and no
// swipe behind them (see the mapping table in MappedInputManager.cpp). A slot
// labelled with the bare direction is therefore telling the reader to press
// something that does not exist, and on the screens that do it the pair only
// duplicates the side keys' scrolling anyway, so drop it and let the labels that
// remain share out its width. A slot whose Left/Right carries a real ACTION
// (Renew, Retry, "-" / "+") is untouched: it names what the tap does rather than
// which button to press. Compared against the translated strings rather than the
// English literals so the rule holds in every language.
const char* dropIfBareDirection(const char* s) {
  if (!hasLabel(s)) return s;
  if (strcmp(s, tr(STR_DIR_LEFT)) == 0 || strcmp(s, tr(STR_DIR_RIGHT)) == 0) return "";
  return s;
}

// Set from the render task just before the activity paints (ActivityManager),
// read by the paint itself. Both run on the render task, so no locking here --
// unlike the slot table, which the input task also reads.
bool confirmRedundant = false;

}  // namespace

void ActionBar::draw(GfxRenderer& renderer, const int barHeight, const int fontId, const bool rounded, const char* btn1,
                     const char* btn2, const char* btn3, const char* btn4) {
  clear();
  if (barHeight <= 0) {
    return;
  }

  // Slot 1 is Confirm (kSlotButtons). Screens whose highlighted row or tile
  // already runs Confirm when tapped drop it here rather than at every call
  // site, and the labels that remain share out its width below.
  const char* labels[4] = {btn1, confirmRedundant ? "" : btn2, dropIfBareDirection(btn3), dropIfBareDirection(btn4)};
  int shown = 0;
  for (const char* label : labels) {
    if (hasLabel(label)) shown++;
  }
  if (shown == 0) {
    return;
  }

  // Logical frame, deliberately unlike the other boards' drawButtonHints (which
  // force portrait so each label lands under its physical button). These are
  // tap targets: they belong where the reader is looking, and the hit-test
  // reads MappedInputManager::wasTapPoint(), which is in this same frame.
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int slotH = barHeight - BAR_TOP_PAD;
  const int slotY = screenH - slotH;

  // The labels always fill the strip between them, whatever their number: one
  // lone Back spans the whole bar, two take half each, four take a quarter. No
  // upper bound -- a screen with one button should give that button the whole
  // width rather than leave dead glass either side of it.
  const int available = screenW - SIDE_MARGIN * 2 - SLOT_GAP * (shown - 1);
  const int slotW = available / shown;
  if (slotW < 1) return;  // absurdly narrow screen; draw nothing rather than garbage

  const int totalW = slotW * shown + SLOT_GAP * (shown - 1);
  int x = (screenW - totalW) / 2;

  const int lineHeight = renderer.getLineHeight(fontId);
  const int textY = slotY + (slotH - lineHeight) / 2;

  int drawn = 0;
  for (int i = 0; i < 4; i++) {
    if (!hasLabel(labels[i])) continue;

    if (rounded) {
      // Top corners only: the buttons are flush with the bottom screen edge, so
      // a rounded bottom pair just carves a sliver out of each one. Same reason
      // Lyra's own hint boxes round only the top (LyraTheme::drawButtonHints).
      constexpr int CORNER_RADIUS = 6;
      renderer.fillRoundedRect(x, slotY, slotW, slotH, CORNER_RADIUS, /*roundTopLeft=*/true, /*roundTopRight=*/true,
                               /*roundBottomLeft=*/false, /*roundBottomRight=*/false, Color::White);
      renderer.drawRoundedRect(x, slotY, slotW, slotH, 1, CORNER_RADIUS, /*roundTopLeft=*/true, /*roundTopRight=*/true,
                               /*roundBottomLeft=*/false, /*roundBottomRight=*/false, /*state=*/true);
    } else {
      renderer.fillRect(x, slotY, slotW, slotH, false);
      renderer.drawRect(x, slotY, slotW, slotH);
    }

    // Truncate rather than overflow the box: a translated label ("Aktualisieren")
    // is far wider than "Retry" and would otherwise run into the next slot.
    const std::string text = renderer.truncatedText(fontId, labels[i], slotW - 8);
    const int textWidth = renderer.getTextWidth(fontId, text.c_str());
    renderer.drawText(fontId, x + (slotW - textWidth) / 2, textY, text.c_str());

    // The published target is deliberately larger than the drawn box: the whole
    // bar height, and half the inter-slot gap on each side. A finger on e-ink
    // gets one shot per refresh, so a near-miss on the border should still
    // count. Half the gap means neighbouring targets meet but never overlap.
    int hitX = x - SLOT_GAP / 2;
    if (hitX < 0) hitX = 0;
    int hitW = slotW + SLOT_GAP;
    if (hitX + hitW > screenW) hitW = screenW - hitX;

    taskENTER_CRITICAL(&slotsMux);
    slots[drawn] = {
        static_cast<int16_t>(hitX),      static_cast<int16_t>(screenH - barHeight), static_cast<int16_t>(hitW),
        static_cast<int16_t>(barHeight), static_cast<uint8_t>(kSlotButtons[i]),     true};
    taskEXIT_CRITICAL(&slotsMux);
    drawn++;

    x += slotW + SLOT_GAP;
  }
}

bool ActionBar::hitTest(const int lx, const int ly, MappedInputManager::Button& outButton) {
  bool hit = false;
  taskENTER_CRITICAL(&slotsMux);
  for (const auto& slot : slots) {
    if (!slot.active) continue;
    if (lx >= slot.x && lx < slot.x + slot.w && ly >= slot.y && ly < slot.y + slot.h) {
      outButton = static_cast<MappedInputManager::Button>(slot.button);
      hit = true;
      break;
    }
  }
  taskEXIT_CRITICAL(&slotsMux);
  return hit;
}

void ActionBar::setConfirmRedundant(const bool redundant) { confirmRedundant = redundant; }

void ActionBar::clear() {
  taskENTER_CRITICAL(&slotsMux);
  for (auto& slot : slots) {
    slot.active = false;
  }
  taskEXIT_CRITICAL(&slotsMux);
}

#endif  // FREEINK_DEVICE_X4PRO
