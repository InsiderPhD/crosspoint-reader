#include "BaseTheme.h"

#include <BluetoothHIDManager.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "I18n.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/bluetooth.h"
#include "components/icons/bluetoothoff.h"
#include "components/icons/bookmark.h"
#include "components/icons/remotecontrol.h"
#include "fontIds.h"
#include "util/HeaderDateUtils.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;
constexpr int subtitleY = 738;
constexpr int bookmarkStatusIconWidth = 16;
constexpr int bookmarkStatusIconHeight = 14;
constexpr int bookmarkStatusIconTopCrop = 2;
// Bluetooth status glyphs (Tabler bluetooth-off / bluetooth / remote-control),
// 16x16 1bpp with convert_icon.py polarity (0 = ink), drawn via renderer.drawIcon.
// Sized to match the bookmark glyph so the status bar reads as one row.
constexpr int btStatusIconSize = 16;

// Blits the 16x16 bookmark glyph (top rows cropped) as 1bpp pixels at (x, y).
void drawBookmarkStatusIcon(const GfxRenderer& renderer, const int x, const int y) {
  constexpr int bytesPerRow = bookmarkStatusIconWidth / 8;
  for (int row = 0; row < bookmarkStatusIconHeight; ++row) {
    for (int col = 0; col < bookmarkStatusIconWidth; ++col) {
      const uint8_t byte = BookmarkStatusIcon[(row + bookmarkStatusIconTopCrop) * bytesPerRow + col / 8];
      const uint8_t mask = 1U << (7 - (col % 8));
      renderer.drawPixel(x + col, y + row, (byte & mask) != 0);
    }
  }
}

// Helper: draw battery icon at given position
void drawBatteryIcon(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight, uint16_t percentage) {
  // Draw battery outline (shared code)
  BaseTheme::drawBatteryOutline(renderer, x, y, battWidth, rectHeight);

  const bool charging = gpio.isUsbConnected();

  // The +1 is to round up, so that we always fill at least one pixel
  const int maxFillWidth = battWidth - 5;
  const int fillHeight = rectHeight - 4;
  if (maxFillWidth <= 0 || fillHeight <= 0) {
    return;
  }
  int filledWidth = percentage * maxFillWidth / 100 + 1;
  if (filledWidth > maxFillWidth) {
    filledWidth = maxFillWidth;
  }

  // When charging, ensure minimum fill so lightning bolt is fully visible
  constexpr int minFillForBolt = 8;
  if (charging && filledWidth < minFillForBolt) {
    filledWidth = std::min(minFillForBolt, maxFillWidth);
  }

  renderer.fillRect(x + 2, y + 2, filledWidth, fillHeight);

  // Draw lightning bolt when charging (white/inverted on black fill for visibility)
  if (charging) {
    BaseTheme::drawBatteryLightningBolt(renderer, x + 4, y + 2);
  }
}
}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rectHeight - 5);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, false);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, false);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, false);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, false);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, false);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, false);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, false);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, false);
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + BaseTheme::batteryPercentSpacing + BaseMetrics::values.batteryWidth,
                      rect.y, percentageText.c_str());
  }

  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const uint16_t percentage = powerManager.getBatteryPercentage();
  const int y = rect.y + 6;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - BaseTheme::batteryPercentSpacing, rect.y,
                      percentageText.c_str());
  }

  // Icon is already at correct position from rect.x
  drawBatteryIcon(renderer, rect.x, y, BaseMetrics::values.batteryWidth, rect.height, percentage);
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  LOG_DBG("UI", "Drawing progress bar: current=%u, total=%u, percent=%d", current, total, percent);
  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
#if FREEINK_DEVICE_X4PRO
  // No front buttons to label on the X4 Pro; the strip is reclaimed by
  // buttonHintsHeight = 0 in the metrics table.
  return;
#endif
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  constexpr int buttonY = BaseMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {25, 130, 245, 350};
  constexpr int x3ButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (gpio.sideKeysAreLeftRight()) {
    // X3/X4 Pro layout: one key each side of the screen — top label on the left
    // edge, bottom label on the right edge, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = buttonMargin;
      renderer.fillRect(leftX, x3ButtonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonMargin - buttonWidth;
      renderer.fillRect(rightX, x3ButtonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    constexpr int topButtonY = 345;
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonMargin - buttonWidth;

    // Fill each box white first so reader/page content underneath doesn't bleed through the hint
    // (these boxes sit over text at the right edge). Matches drawButtonHints' opaque background.
    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.fillRect(x, topButtonY, buttonWidth, buttonHeight, false);
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.fillRect(x, topButtonY + buttonHeight, buttonWidth, buttonHeight, false);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

void BaseTheme::drawPowerButtonHint(GfxRenderer& renderer, const char* label) const {
  if (label == nullptr || label[0] == '\0') return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonMargin = 4;
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // align with Up/Down side hints
  constexpr int buttonHeight = 80;                                       // fixed size, same as the side boxes

  // Same bordered-box look as drawButtonHints (UI_10 font, white fill + outline), but the box
  // is oriented vertically and the label rotated 90° CW for the side-mounted Power button.
  // Placed to track the physical Power button: top-right on the X3, and on the right edge just
  // above the Up/Down side hints on the X4.
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
  const int textHeight = renderer.getTextHeight(UI_10_FONT_ID);

  if (gpio.deviceIsX3()) {
    // X3's Power button is on the top edge: a horizontal box there with a normal (right-side-up)
    // label. Same physical spot as the inverted version, but drawn normally so the text isn't
    // flipped (mirrored coordinates land it on the same top-edge corner).
    const int boxW = textWidth + 14;
    const int boxH = 40;  // match the 40px thickness of the other hint boxes
    const int bx = screenWidth - buttonMargin - boxW;
    const int by = buttonMargin;
    renderer.fillRect(bx, by, boxW, boxH, false);
    renderer.drawRect(bx, by, boxW, boxH);
    renderer.drawText(UI_10_FONT_ID, bx + 7, by + (boxH - textHeight) / 2, label);
    return;
  }

  // X4: Power button is on the right, above the Up/Down side hints. Vertical box,
  // label rotated 90° CW.
  const int x = screenWidth - buttonMargin - buttonWidth;
#if FREEINK_DEVICE_X4PRO
  // X4 Pro: the side hints use the X3 placement (right-edge box at y=155), so anchor
  // just above that box — the X4's 345/140 anchor would land the power box on top of it.
  constexpr int sideHintsTopY = 155;
  constexpr int gap = 20;
#else
  // drawSideButtonHints stacks the X4's boxes starting at topButtonY = 345.
  constexpr int sideHintsTopY = 345;
  constexpr int gap = 140;
#endif
  const int y = sideHintsTopY - gap - buttonHeight;
  renderer.fillRect(x, y, buttonWidth, buttonHeight, false);
  renderer.drawRect(x, y, buttonWidth, buttonHeight);
  const int textX = x + (buttonWidth - textHeight) / 2;
  const int textY = y + (buttonHeight + textWidth) / 2;
  renderer.drawTextRotated90CW(UI_10_FONT_ID, textX, textY, label);
}

BaseTheme::ListGeometry BaseTheme::listGeometry(const Rect& rect, int selectedIndex, bool hasSubtitle) const {
  const ThemeMetrics& m = themeMetrics();
  const int rowHeight = hasSubtitle ? m.listWithSubtitleRowHeight : m.listRowHeight;
  int pageItems = rect.height / rowHeight;
  if (pageItems < 1) pageItems = 1;
  // selectedIndex may be -1 (list drawn unfocused, e.g. while a tab bar holds
  // the selection); matches drawList's historical -1/pageItems truncation to
  // page 0.
  const int pageStart = selectedIndex <= 0 ? 0 : selectedIndex / pageItems * pageItems;
  return {rowHeight, pageItems, pageStart};
}

int BaseTheme::hitTestList(const Rect& rect, int itemCount, int selectedIndex, bool hasSubtitle, int lx, int ly) const {
  if (itemCount <= 0) {
    return -1;
  }
  if (lx < rect.x || lx >= rect.x + rect.width) {
    return -1;
  }
  const ListGeometry geo = listGeometry(rect, selectedIndex, hasSubtitle);
  // The dead strip between the last full row and rect's bottom edge is a miss.
  if (ly < rect.y || ly >= rect.y + geo.pageItems * geo.rowHeight) {
    return -1;
  }
  const int index = geo.pageStart + (ly - rect.y) / geo.rowHeight;
  return index < itemCount ? index : -1;
}

int BaseTheme::tabCellWidth(const GfxRenderer& renderer, const TabInfo& tab) const {
  int w = renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (tab.hasBadge) {
    w += TAB_BADGE_GAP + TAB_BADGE_SIZE;
  }
  return w;
}

int BaseTheme::computeTabScrollOffset(const GfxRenderer& renderer, const Rect& rect,
                                      const std::vector<TabInfo>& tabs) const {
  const ThemeMetrics& m = themeMetrics();
  const int startX = rect.x + m.contentSidePadding;
  const int viewRight = rect.x + rect.width - m.contentSidePadding;

  // Measure the natural (unscrolled) layout and the selected tab's bounds so
  // the ribbon can scroll horizontally when the tabs overflow, keeping the
  // selected tab's full label readable.
  int naturalX = startX;
  int selLeft = startX;
  int selRight = startX;
  for (const auto& tab : tabs) {
    const int w = tabCellWidth(renderer, tab);
    if (tab.selected) {
      selLeft = naturalX;
      selRight = naturalX + w;
    }
    naturalX += w + m.tabSpacing;
  }
  const int totalWidth = naturalX - m.tabSpacing - startX;
  const int viewWidth = viewRight - startX;
  // Keep a sliver of the neighbouring tab visible so it's obvious more tabs
  // exist. Clamping still makes the genuine first/last tab sit flush.
  constexpr int PEEK = 28;

  int offset = 0;
  if (totalWidth > viewWidth) {
    if (selRight - offset > viewRight - PEEK) offset = selRight - (viewRight - PEEK);
    if (selLeft - offset < startX + PEEK) offset = selLeft - (startX + PEEK);
    const int maxOffset = totalWidth - viewWidth;
    if (offset < 0) offset = 0;
    if (offset > maxOffset) offset = maxOffset;
  }
  return offset;
}

int BaseTheme::hitTestTabBar(const GfxRenderer& renderer, const Rect& rect, const std::vector<TabInfo>& tabs, int lx,
                             int ly) const {
  if (tabs.empty()) {
    return -1;
  }
  if (ly < rect.y || ly >= rect.y + rect.height) {
    return -1;
  }
  const ThemeMetrics& m = themeMetrics();
  const int startX = rect.x + m.contentSidePadding;
  const int viewRight = rect.x + rect.width - m.contentSidePadding;
  const int offset = computeTabScrollOffset(renderer, rect, tabs);
  // Widen each cell by half the inter-tab spacing so gutter taps snap to the
  // nearest tab (same rationale as the keyboard's pitch indexing).
  const int half = m.tabSpacing / 2;
  int currentX = startX - offset;
  for (int i = 0; i < static_cast<int>(tabs.size()); i++) {
    const int w = tabCellWidth(renderer, tabs[i]);
    const bool visible = currentX + w >= startX && currentX <= viewRight;
    if (visible && lx >= currentX - half && lx < currentX + w + half) {
      return i;
    }
    currentX += w + m.tabSpacing;
  }
  return -1;
}

int BaseTheme::hitTestButtonMenu(const Rect& rect, int buttonCount, int lx, int ly) const {
  const ThemeMetrics& m = themeMetrics();
  if (lx < rect.x + m.contentSidePadding || lx >= rect.x + rect.width - m.contentSidePadding) {
    return -1;
  }
  const int pitch = m.menuRowHeight + m.menuSpacing;
  const int rel = ly - rect.y - buttonMenuTopOffset();
  if (rel < 0) {
    return -1;
  }
  const int index = rel / pitch;
  // Taps in the spacing gap between tiles are a miss.
  if (index >= buttonCount || rel % pitch >= m.menuRowHeight) {
    return -1;
  }
  return index;
}

int BaseTheme::hitTestRecentBookCover(const Rect& rect, const int slotCount, const int lx, const int ly) const {
  if (slotCount <= 0) {
    return -1;
  }
  if (lx < rect.x || lx >= rect.x + rect.width || ly < rect.y || ly >= rect.y + rect.height) {
    return -1;
  }
  const ThemeMetrics& m = themeMetrics();
  const int columns = m.homeRecentBooksCount;
  if (columns <= 1) {
    return 0;
  }
  const int tileWidth = (rect.width - 2 * m.contentSidePadding) / columns;
  if (tileWidth <= 0) {
    return 0;
  }
  int column = (lx - rect.x - m.contentSidePadding) / tileWidth;
  if (column < 0) {
    column = 0;
  }
  return column < slotCount ? column : -1;
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  const ListGeometry geo = listGeometry(rect, selectedIndex, rowSubtitle != nullptr);
  const int rowHeight = geo.rowHeight;
  const int pageItems = geo.pageItems;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection
  int contentWidth = rect.width - 5;
  if (selectedIndex >= 0) {
    renderer.fillRect(0, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  // Draw all items
  const int pageStartIndex = geo.pageStart;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Compute value width first so the title gets all remaining space
    std::string valueText;
    int valueTextWidth = 0;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      }
    }
    const int valueReservation = valueTextWidth > 0 ? valueTextWidth + BaseMetrics::values.contentSidePadding : 0;
    const int textWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2 - valueReservation;

    // Draw name
    auto itemName = rowTitle(i);
    auto font = (rowSubtitle != nullptr) ? UI_12_FONT_ID : UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), textWidth);
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(font, item.c_str());
      const int lineH = renderer.getLineHeight(font);
      const int tx = rect.x + BaseMetrics::values.contentSidePadding;
      for (int py = itemY; py < itemY + lineH; py++)
        for (int px = tx; px < tx + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 30, subtitle.c_str(),
                        i != selectedIndex);
    }

    if (valueTextWidth > 0) {
      // Draw value right-aligned, vertically centred in the row
      const int valueY =
          rowSubtitle != nullptr ? itemY + (rowHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 : itemY;
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        valueY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const char* powerButtonHintLabel) const {
  // On the X3 the Power-button hint box (e.g. "Sort") is pinned to the top-right corner where the
  // battery renders, overlapping it. When that hint is present, shift the battery group (icon + %)
  // left so it clears the box. The shift equals the box width (mirrors drawPowerButtonHint's X3
  // box: textWidth + 14), which leaves an 8px gap. X4 draws the hint on the side edge — no shift.
  int powerHintShift = 0;
  if (powerButtonHintLabel != nullptr && powerButtonHintLabel[0] != '\0' && gpio.deviceIsX3()) {
    powerHintShift = renderer.getTextWidth(UI_10_FONT_ID, powerButtonHintLabel) + 14;
  }

  // Hide last battery draw (extend the cleared strip to cover the shifted position)
  constexpr int maxBatteryWidth = 80;
  renderer.fillRect(rect.x + rect.width - maxBatteryWidth - powerHintShift, rect.y + 5,
                    maxBatteryWidth + powerHintShift, BaseMetrics::values.batteryHeight + 10, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - BaseMetrics::values.batteryWidth - powerHintShift;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, BaseMetrics::values.batteryWidth, BaseMetrics::values.batteryHeight},
                   showBatteryPercentage);

  // Date on the left of the header's top line. Only fires for screens that go
  // through drawHeader (home/library/recent/stats/settings) — the reader uses
  // drawStatusBar and is therefore unaffected.
  HeaderDateUtils::drawTopLine(const_cast<GfxRenderer&>(renderer));

  if (title) {
    int padding = rect.width - batteryX + BaseMetrics::values.batteryWidth;
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title,
                                                 rect.width - padding * 2 - BaseMetrics::values.contentSidePadding * 2,
                                                 EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 5, truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(
        SMALL_FONT_ID, subtitle, rect.width - BaseMetrics::values.contentSidePadding * 2, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      rect.x + rect.width - BaseMetrics::values.contentSidePadding - truncatedSubtitleWidth, subtitleY,
                      truncatedSubtitle.c_str(), true);
  }
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int pad = BaseMetrics::values.contentSidePadding;
  const int spacing = BaseMetrics::values.tabSpacing;
  const int startX = rect.x + pad;
  const int viewRight = rect.x + rect.width - pad;

  // Pass 1 (measure + scroll) lives in computeTabScrollOffset, shared with
  // hitTestTabBar so a tap can never land on a different tab than was drawn.
  const int offset = computeTabScrollOffset(renderer, rect, tabs);

  // Pass 2: draw shifted by the scroll offset, skipping tabs fully off-view.
  int currentX = startX - offset;
  for (const auto& tab : tabs) {
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int cellWidth = tabCellWidth(renderer, tab);

    if (currentX + cellWidth >= startX && currentX <= viewRight) {
      if (tab.selected) {
        if (selected) {
          renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
        } else {
          renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
        }
      }
      renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected),
                        tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      if (tab.hasBadge) {
        const int dotX = currentX + textWidth + TAB_BADGE_GAP;
        const int dotY = rect.y + (lineHeight - TAB_BADGE_SIZE) / 2;
        renderer.fillRect(dotX, dotY, TAB_BADGE_SIZE, TAB_BADGE_SIZE, !(tab.selected && selected));
      }
    }

    currentX += cellWidth + spacing;
  }
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          LOG_DBG("THEME", "Rendering bmp");

          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            LOG_DBG("THEME", "Drawing selection");
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (hasContinueReading) {
    const std::string& lastBookTitle = recentBooks[0].title;
    const std::string& lastBookAuthor = recentBooks[0].author;

    // Invert text colors based on selection state:
    // - With cover: selected = white text on black box, unselected = black text on white box
    // - Without cover: selected = white text on black card, unselected = black text on white card

    auto lines = renderer.wrappedText(UI_12_FONT_ID, lastBookTitle.c_str(), bookWidth - 40, 3);

    // Book title text
    int totalTextHeight = renderer.getLineHeight(UI_12_FONT_ID) * static_cast<int>(lines.size());
    if (!lastBookAuthor.empty()) {
      totalTextHeight += renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    }

    // Vertically center the title block within the card
    int titleYStart = bookY + (bookHeight - totalTextHeight) / 2;

    const auto truncatedAuthor = lastBookAuthor.empty()
                                     ? std::string{}
                                     : renderer.truncatedText(UI_10_FONT_ID, lastBookAuthor.c_str(), bookWidth - 40);

    // If cover image was rendered, draw box behind title and author
    if (coverRendered) {
      constexpr int boxPadding = 8;
      // Calculate the max text width for the box
      int maxTextWidth = 0;
      for (const auto& line : lines) {
        const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str());
        if (lineWidth > maxTextWidth) {
          maxTextWidth = lineWidth;
        }
      }
      if (!truncatedAuthor.empty()) {
        const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedAuthor.c_str());
        if (authorWidth > maxTextWidth) {
          maxTextWidth = authorWidth;
        }
      }

      const int boxWidth = maxTextWidth + boxPadding * 2;
      const int boxHeight = totalTextHeight + boxPadding * 2;
      const int boxX = rect.x + (rect.width - boxWidth) / 2;
      const int boxY = titleYStart - boxPadding;

      // Draw box (inverted when selected: black box instead of white)
      renderer.fillRect(boxX, boxY, boxWidth, boxHeight, bookSelected);
      // Draw border around the box (inverted when selected: white border instead of black)
      renderer.drawRect(boxX, boxY, boxWidth, boxHeight, !bookSelected);
    }

    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, titleYStart, line.c_str(), !bookSelected);
      titleYStart += renderer.getLineHeight(UI_12_FONT_ID);
    }

    if (!truncatedAuthor.empty()) {
      titleYStart += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, titleYStart, truncatedAuthor.c_str(), !bookSelected);
    }

    // "Continue Reading" label at the bottom, with progress percentage if known
    char continueLabel[48];
    if (recentBooks[0].progressPercent >= 0) {
      snprintf(continueLabel, sizeof(continueLabel), "%s  %d%%", tr(STR_CONTINUE_READING),
               static_cast<int>(recentBooks[0].progressPercent));
    } else {
      snprintf(continueLabel, sizeof(continueLabel), "%s", tr(STR_CONTINUE_READING));
    }
    const int continueY = bookY + bookHeight - renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2;
    if (coverRendered) {
      // Draw box behind "Continue Reading" text (inverted when selected: black box instead of white)
      const char* continueText = continueLabel;
      const int continueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, continueText);
      constexpr int continuePadding = 6;
      const int continueBoxWidth = continueTextWidth + continuePadding * 2;
      const int continueBoxHeight = renderer.getLineHeight(UI_10_FONT_ID) + continuePadding;
      const int continueBoxX = rect.x + (rect.width - continueBoxWidth) / 2;
      const int continueBoxY = continueY - continuePadding / 2;
      renderer.fillRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, bookSelected);
      renderer.drawRect(continueBoxX, continueBoxY, continueBoxWidth, continueBoxHeight, !bookSelected);
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueText, !bookSelected);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, continueY, continueLabel, !bookSelected);
    }
  } else {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "No open book");
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), "Start reading below");
  }
}

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    const int tileY = BaseMetrics::values.verticalSpacing + rect.y +
                      static_cast<int>(i) * (BaseMetrics::values.menuRowHeight + BaseMetrics::values.menuSpacing);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + BaseMetrics::values.contentSidePadding, tileY,
                        rect.width - BaseMetrics::values.contentSidePadding * 2, BaseMetrics::values.menuRowHeight);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + (rect.width - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (BaseMetrics::values.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int margin = 15;
  // Scale y position proportionally to screen height (7.5% from top)
  const int y = static_cast<int>(renderer.getScreenHeight() * 0.075f);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRect(x - 2, y - 2, w + 4, h + 4, true);  // frame thickness 2
  renderer.fillRect(x, y, w, h, false);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, true, EpdFontFamily::BOLD);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  constexpr int barHeight = 4;
  const int barWidth = layout.width - 30;  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - 10;

  int fillWidth = barWidth * progress / 100;

  renderer.fillRect(barX, barY, fillWidth, barHeight, true);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

static std::string formatTimeLeft(uint32_t totalSeconds) {
  if (totalSeconds < 60) return "~1m";
  const uint32_t minutes = (totalSeconds + 30) / 60;
  char buf[12];
  if (minutes < 60) {
    snprintf(buf, sizeof(buf), "~%um", static_cast<unsigned>(minutes));
  } else {
    const uint32_t hours = minutes / 60;
    const uint32_t mins = minutes % 60;
    if (mins == 0) {
      snprintf(buf, sizeof(buf), "~%uh", static_cast<unsigned>(hours));
    } else {
      snprintf(buf, sizeof(buf), "~%uh%um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
    }
  }
  return std::string(buf);
}

void BaseTheme::drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage,
                              const int pageCount, const std::string& bookTitle, const std::string& chapterTitle,
                              const uint32_t chapterTimeLeftSeconds, const uint32_t bookTimeLeftSeconds,
                              const bool isPageBookmarked, const int paddingBottom, const int textYOffset,
                              const std::string& centerOverride) const {
  using CPS = CrossPointSettings;
  auto metrics = UITheme::getInstance().getMetrics();
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  // getStatusBarHeight() now includes statusBarContentGap plus the user's extra top margin to
  // reserve breathing room above the bar in the body layout; add both back here so the bar text
  // stays anchored to the bottom and the reserved space stays empty.
  const uint8_t topMargin = SETTINGS.statusBarTopMargin > CPS::STATUS_BAR_TOP_MARGIN_MAX
                                ? CPS::STATUS_BAR_TOP_MARGIN_MAX
                                : SETTINGS.statusBarTopMargin;
  const int textY = screenHeight - UITheme::getInstance().getStatusBarHeight() - orientedMarginBottom - paddingBottom -
                    4 + metrics.statusBarContentGap + topMargin;
  const int contentY = textY - textYOffset;

  // --- Progress fill bar (full width, independent of the positioned text elements) ---
  if (SETTINGS.statusBarProgressBar != CPS::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    const int progressBarMaxWidth = screenWidth - orientedMarginLeft - orientedMarginRight;
    const int progressBarY =
        screenHeight - orientedMarginBottom - ((SETTINGS.statusBarProgressBarThickness + 1) * 2) - paddingBottom;
    size_t progress;
    if (SETTINGS.statusBarProgressBar == CPS::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS) {
      progress = static_cast<size_t>(bookProgress);
    } else {
      progress = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) * 100 : 0;
    }
    const int barWidth = progressBarMaxWidth * progress / 100;
    renderer.fillRect(orientedMarginLeft, progressBarY, barWidth, ((SETTINGS.statusBarProgressBarThickness + 1) * 2),
                      true);
  }

  const bool showBatteryPercentage = SETTINGS.hideBatteryPercentage == CPS::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  constexpr int elementGap = 10;

  // --- Build the ordered element list, each with its Hide/Left/Middle/Right position ---
  // kind: 0 = text, 1 = battery icon, 2 = bookmark icon, 3 = bluetooth icon.
  struct SbItem {
    uint8_t pos;
    uint8_t kind;
    std::string text;
    int width;
    const uint8_t* icon = nullptr;  // kind 3 only
  };
  std::vector<SbItem> items;
  items.reserve(11);

  auto validPos = [](const uint8_t p) { return p != CPS::SB_POS_HIDE && p < CPS::STATUS_BAR_POS_COUNT; };
  auto addText = [&](const uint8_t pos, std::string s) {
    if (!validPos(pos) || s.empty()) return;
    const int w = renderer.getTextWidth(SMALL_FONT_ID, s.c_str());
    if (w <= 0) return;
    items.push_back(SbItem{pos, 0, std::move(s), w});
  };

  // Battery (icon + optional percentage)
  if (validPos(SETTINGS.statusBarBatteryPos)) {
    int w = metrics.batteryWidth;
    if (showBatteryPercentage) {
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%u%%", powerManager.getBatteryPercentage());
      w += batteryPercentSpacing + renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
    }
    items.push_back(SbItem{SETTINGS.statusBarBatteryPos, 1, std::string(), w});
  }
  // Bookmark indicator (only on a bookmarked page)
  if (isPageBookmarked && validPos(SETTINGS.statusBarBookmarkPos)) {
    items.push_back(SbItem{SETTINGS.statusBarBookmarkPos, 2, std::string(), bookmarkStatusIconWidth});
  }
  // Titles — suppressed while an override banner (auto page-turn) owns the centre.
  if (centerOverride.empty()) {
    addText(SETTINGS.statusBarBookTitlePos, bookTitle);
    addText(SETTINGS.statusBarChapterTitlePos, chapterTitle);
  }
  // Chapter page count (current / total) then book percentage — this order keeps
  // the classic right-cluster reading of "12/32  45%" when both are placed together.
  {
    char b[24];
    snprintf(b, sizeof(b), "%d/%d", currentPage, pageCount);
    addText(SETTINGS.statusBarChapterPagePos, b);
  }
  {
    char b[16];
    snprintf(b, sizeof(b), "%.0f%%", bookProgress);
    addText(SETTINGS.statusBarBookPercentPos, b);
  }
  // Time-left (book and chapter are independent elements)
  if (bookTimeLeftSeconds > 0) addText(SETTINGS.statusBarBookTimeLeftPos, formatTimeLeft(bookTimeLeftSeconds));
  if (chapterTimeLeftSeconds > 0) addText(SETTINGS.statusBarChapterTimeLeftPos, formatTimeLeft(chapterTimeLeftSeconds));
  // Clock (X3 only — DS3231 RTC)
  if (validPos(SETTINGS.statusBarClockPos) && halClock.isAvailable()) {
    char timeBuf[9];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
      addText(SETTINGS.statusBarClockPos, timeBuf);
    }
  }
  // Bluetooth remote state, shown as an icon:
  //   off (stack down) -> bluetooth-off, on but no remote -> bluetooth, connected -> remote-control.
  if (validPos(SETTINGS.statusBarBluetoothPos)) {
    const auto& btMgr = BluetoothHIDManager::getInstance();
    const uint8_t* btIcon = !btMgr.isEnabled()           ? BluetoothoffIcon
                            : btMgr.hasConnectedDevice() ? RemotecontrolIcon
                                                         : BluetoothIcon;
    items.push_back(SbItem{SETTINGS.statusBarBluetoothPos, 3, std::string(), btStatusIconSize, btIcon});
  }
  // Auto page-turn banner takes the middle cluster.
  if (!centerOverride.empty()) addText(CPS::SB_POS_MIDDLE, centerOverride);

  // --- Cluster layout: left grows rightward, right grows leftward, middle centres ---
  auto zoneWidth = [&items](const uint8_t zone) {
    int total = 0;
    int count = 0;
    for (const auto& it : items)
      if (it.pos == zone) {
        total += it.width;
        count++;
      }
    if (count > 1) total += elementGap * (count - 1);
    return total;
  };

  const int leftEdge = metrics.statusBarHorizontalMargin + orientedMarginLeft + 1;
  const int rightEdge = screenWidth - metrics.statusBarHorizontalMargin - orientedMarginRight;
  const int innerWidth = rightEdge - leftEdge;

  // Over-wide clusters (long titles) get their widest text element truncated to fit.
  auto fitZone = [&](const uint8_t zone, const int available) {
    for (int guard = 0; guard < 4 && zoneWidth(zone) > available; guard++) {
      const int over = zoneWidth(zone) - available;
      SbItem* widest = nullptr;
      for (auto& it : items)
        if (it.pos == zone && it.kind == 0 && (!widest || it.width > widest->width)) widest = &it;
      if (!widest || widest->width <= 8) break;
      const int newW = std::max(8, widest->width - over);
      widest->text = renderer.truncatedText(SMALL_FONT_ID, widest->text.c_str(), newW);
      widest->width = renderer.getTextWidth(SMALL_FONT_ID, widest->text.c_str());
    }
  };

  fitZone(CPS::SB_POS_LEFT, (innerWidth * 45) / 100);
  fitZone(CPS::SB_POS_RIGHT, (innerWidth * 45) / 100);

  const int leftW = zoneWidth(CPS::SB_POS_LEFT);
  const int rightW = zoneWidth(CPS::SB_POS_RIGHT);
  const int leftEnd = leftEdge + leftW;
  const int rightStart = rightEdge - rightW;

  // Middle is centred on the screen but never overlaps a side cluster; if the
  // clusters leave no room it simply uses whatever gap remains.
  const int middleAvail = (leftW == 0 && rightW == 0) ? innerWidth : std::max(8, rightStart - leftEnd - 2 * elementGap);
  fitZone(CPS::SB_POS_MIDDLE, middleAvail);
  const int middleW = zoneWidth(CPS::SB_POS_MIDDLE);
  int middleX = (screenWidth - middleW) / 2;
  middleX = std::max(middleX, leftEnd + (leftW > 0 ? elementGap : 0));
  middleX = std::min(middleX, rightStart - (rightW > 0 ? elementGap : 0) - middleW);
  if (middleX < leftEnd) middleX = leftEnd;

  // Running cursor per cluster (indexed by SB_POS_* value; HIDE slot unused).
  int cursor[CPS::STATUS_BAR_POS_COUNT] = {0, leftEdge, middleX, rightStart};
  for (const auto& it : items) {
    if (it.pos == CPS::SB_POS_HIDE || it.pos >= CPS::STATUS_BAR_POS_COUNT) continue;
    const int x = cursor[it.pos];
    cursor[it.pos] += it.width + elementGap;
    if (it.kind == 1) {
      GUI.drawBatteryLeft(renderer, Rect{x, contentY, metrics.batteryWidth, metrics.batteryHeight},
                          showBatteryPercentage);
    } else if (it.kind == 2) {
      drawBookmarkStatusIcon(renderer, x, contentY + 5);
    } else if (it.kind == 3) {
      renderer.drawIcon(it.icon, x, contentY + 4, btStatusIconSize, btStatusIconSize);
    } else {
      renderer.drawText(SMALL_FONT_ID, x, contentY, it.text.c_str());
    }
  }
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 1;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth, lineY, thickness, true);
  } else {
    const int hPadding = 6;
    const int lineW = textWidth + hPadding * 2;
    renderer.drawLine(rect.x + (rect.width - lineW) / 2, lineY, rect.x + (rect.width + lineW) / 2, lineY, thickness,
                      true);
  }
}

void BaseTheme::drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                                const char* secondaryLabel, const KeyboardKeyType keyType,
                                const bool inactiveSelection) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int cr = metrics.keyboardKeyCornerRadius;

  if (isSelected) {
    if (inactiveSelection) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height, 2, true);
      }
    } else if (keyType == KeyboardKeyType::Disabled) {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::LightGray);
      } else {
        renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
      }
    } else {
      if (cr > 0) {
        renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, cr, Color::Black);
      } else {
        renderer.fillRect(rect.x, rect.y, rect.width, rect.height, true);
      }
    }
  } else if (keyType == KeyboardKeyType::Shift || keyType == KeyboardKeyType::Mode || keyType == KeyboardKeyType::Del ||
             keyType == KeyboardKeyType::Space || keyType == KeyboardKeyType::Ok ||
             keyType == KeyboardKeyType::Disabled) {
    if (cr > 0) {
      renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, cr, true);
    } else {
      renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
    }
  }

  const bool invert = isSelected && !inactiveSelection;

  if (keyType == KeyboardKeyType::Space) {
    const int lineHalfWidth = rect.width * 3 / 10;
    const int centerX = rect.x + rect.width / 2;
    const int lineY = rect.y + rect.height / 2 + 3;
    renderer.drawLine(centerX - lineHalfWidth, lineY, centerX + lineHalfWidth, lineY, 3, !invert);
    return;
  }

  if (keyType == KeyboardKeyType::Del) {
    const int centerX = rect.x + rect.width / 2;
    const int centerY = rect.y + rect.height / 2;
    const int arrowLen = rect.width / 4;
    const int arrowHead = arrowLen / 2;
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX + arrowLen / 2, centerY, 3, !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY - arrowHead, 3,
                      !invert);
    renderer.drawLine(centerX - arrowLen / 2, centerY, centerX - arrowLen / 2 + arrowHead, centerY + arrowHead, 3,
                      !invert);
    return;
  }

  const bool hasSecondary = secondaryLabel != nullptr && secondaryLabel[0] != '\0';
  const int itemWidth = renderer.getTextWidth(UI_12_FONT_ID, label);
  const int textX = rect.x + (rect.width - itemWidth) / 2;
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2;

  renderer.drawText(UI_12_FONT_ID, textX, textY, label, !invert);

  if (hasSecondary) {
    const int secWidth = renderer.getTextWidth(SMALL_FONT_ID, secondaryLabel);
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - secWidth - 1, rect.y, secondaryLabel, !invert);
  }
}
