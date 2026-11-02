#include "ReleaseNotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReleaseNotes.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MAX_WRAP_LINES = 16;  // per note body; the notes are a few sentences
}

void ReleaseNotesActivity::onEnter() {
  Activity::onEnter();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth() - metrics.contentSidePadding * 2;

  lines.clear();
  lines.reserve(ReleaseNotes::ITEM_COUNT * 6);  // heading + ~4 body lines + gap
  for (size_t i = 0; i < ReleaseNotes::ITEM_COUNT; i++) {
    const auto& item = ReleaseNotes::ITEMS[i];
    for (auto& text : renderer.wrappedText(UI_10_FONT_ID, I18N.get(item.title), width, 2, EpdFontFamily::BOLD)) {
      lines.push_back({std::move(text), true});
    }
    for (auto& text : renderer.wrappedText(UI_10_FONT_ID, I18N.get(item.body), width, MAX_WRAP_LINES)) {
      lines.push_back({std::move(text), false});
    }
    lines.push_back({std::string(), false});  // gap between notes
  }
  if (!lines.empty()) lines.pop_back();

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  linesPerPage = std::max(1, (bottom - top) / lineH);
  firstLine = 0;
  requestUpdate();
}

void ReleaseNotesActivity::onExit() {
  Activity::onExit();
  lines.clear();
  lines.shrink_to_fit();
}

void ReleaseNotesActivity::close() {
  ReleaseNotes::markSeen();
  finish();
}

void ReleaseNotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    close();
    return;
  }
  const int maxFirst = std::max(0, static_cast<int>(lines.size()) - linesPerPage);
  buttonNavigator.onNext([this, maxFirst] {
    const int next = std::min(maxFirst, firstLine + linesPerPage);
    if (next != firstLine) {
      firstLine = next;
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    const int prev = std::max(0, firstLine - linesPerPage);
    if (prev != firstLine) {
      firstLine = prev;
      requestUpdate();
    }
  });
}

void ReleaseNotesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  renderer.clearScreen();

  char title[48];
  snprintf(title, sizeof(title), "%s %s", tr(STR_WHATS_NEW), ReleaseNotes::RELEASE_NOTES_VERSION);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 4;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int end = std::min(static_cast<int>(lines.size()), firstLine + linesPerPage);
  for (int i = firstLine; i < end; i++) {
    const auto& line = lines[i];
    if (!line.text.empty()) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, line.text.c_str(), true,
                        line.heading ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
    y += lineH;
  }

  const bool more = end < static_cast<int>(lines.size());
  const bool less = firstLine > 0;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), less ? tr(STR_DIR_UP) : "", more ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
