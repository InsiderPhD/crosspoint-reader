#include "SleepThemeSelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "sleeptheme/SleepThemeRenderer.h"
#include "util/TouchListNav.h"

void SleepThemeSelectActivity::onEnter() {
  Activity::onEnter();

  discover();

  selectedIndex = 0;
  for (size_t i = 0; i < themes.size(); ++i) {
    if (strncmp(themes[i].c_str(), SETTINGS.sleepTheme, sizeof(SETTINGS.sleepTheme) - 1) == 0) {
      selectedIndex = static_cast<int>(i);
      break;
    }
  }

  requestUpdate();
}

void SleepThemeSelectActivity::onExit() { Activity::onExit(); }

void SleepThemeSelectActivity::discover() {
  themes.clear();
  themes.reserve(8);

  auto root = Storage.open(SleepThemeRenderer::THEMES_DIR);
  if (!root || !root.isDirectory()) {
    LOG_DBG("STS", "No %s directory on SD card", SleepThemeRenderer::THEMES_DIR);
    return;
  }

  root.rewindDirectory();
  char name[128];
  char path[192];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (!entry.isDirectory() || name[0] == '.') continue;
    // The setting holds at most 31 characters; a longer folder could never be selected.
    if (strlen(name) >= sizeof(SETTINGS.sleepTheme)) {
      LOG_INF("STS", "Skipping theme '%s': folder name too long", name);
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s/theme.bin", SleepThemeRenderer::THEMES_DIR, name);
    if (!Storage.exists(path)) continue;
    themes.emplace_back(name);
  }

  std::sort(themes.begin(), themes.end(),
            [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
}

void SleepThemeSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (themes.empty()) return;

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), totalItems(), selectedIndex,
                               /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      handleSelection();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems());
    requestUpdate();
  });

  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, totalItems(), pageItems, selectedIndex)) {
    requestUpdate();
  }
}

void SleepThemeSelectActivity::handleSelection() {
  if (selectedIndex < 0 || selectedIndex >= totalItems()) return;
  {
    RenderLock lock(*this);
    const auto& chosen = themes[static_cast<size_t>(selectedIndex)];
    strncpy(SETTINGS.sleepTheme, chosen.c_str(), sizeof(SETTINGS.sleepTheme) - 1);
    SETTINGS.sleepTheme[sizeof(SETTINGS.sleepTheme) - 1] = '\0';
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::THEME;
    // Only reachable by an explicit choice, so not a hot write path.
    SETTINGS.saveToFile();
  }
  finish();
}

// List body between the header and the button hints. Shared by render() and
// the tap hit-testing so the two can never disagree.
Rect SleepThemeSelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void SleepThemeSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SLEEP_THEME));

  if (themes.empty()) {
    const Rect body = listRect();
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_NO_SLEEP_THEMES), pageWidth - 2 * 24, 4);
    int y = body.y + (body.height - static_cast<int>(lines.size()) * lineHeight) / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true);
      y += lineHeight;
    }
  } else {
    const bool themeMode = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::THEME;
    GUI.drawList(
        renderer, listRect(), totalItems(), selectedIndex,
        [this](int index) -> std::string { return themes[static_cast<size_t>(index)]; }, nullptr, nullptr,
        [this, themeMode](int index) -> std::string {
          const bool active = themeMode && strncmp(themes[static_cast<size_t>(index)].c_str(), SETTINGS.sleepTheme,
                                                   sizeof(SETTINGS.sleepTheme) - 1) == 0;
          return active ? tr(STR_SELECTED) : "";
        },
        true);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), themes.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
