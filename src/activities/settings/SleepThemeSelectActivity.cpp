#include "SleepThemeSelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "SleepThemeDownloadActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/ListPickerActivity.h"
#include "fontIds.h"
#include "sleeptheme/SleepThemeRenderer.h"
#include "util/TouchListNav.h"

void SleepThemeSelectActivity::onEnter() {
  Activity::onEnter();

  discover();
  placeCursor();
  requestUpdate();
}

void SleepThemeSelectActivity::placeCursor() {
  configuredMissing = SETTINGS.sleepTheme[0] != '\0' && !SleepThemeRenderer::isBuiltIn(SETTINGS.sleepTheme);
  selectedIndex = storeIndex();
  for (size_t i = 0; i < themes.size(); ++i) {
    if (strncmp(themes[i].c_str(), SETTINGS.sleepTheme, sizeof(SETTINGS.sleepTheme) - 1) == 0) {
      selectedIndex = static_cast<int>(i);
      configuredMissing = false;
      break;
    }
  }
  if (!configuredMissing && selectedIndex == storeIndex() && !themes.empty()) selectedIndex = 0;
}

void SleepThemeSelectActivity::onExit() { Activity::onExit(); }

void SleepThemeSelectActivity::discover() {
  themes.clear();
  themes.reserve(8);

  auto root = Storage.open(SleepThemeRenderer::THEMES_DIR);
  if (!root || !root.isDirectory()) {
    LOG_DBG("STS", "No %s directory on SD card", SleepThemeRenderer::THEMES_DIR);
    addBuiltIns();
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
  addBuiltIns();
}

// The built-in themes are always choices: the device draws them itself until
// the store's copies are downloaded.
void SleepThemeSelectActivity::addBuiltIns() {
  for (const char* name : SleepThemeRenderer::BUILT_IN_THEMES) {
    if (std::find(themes.begin(), themes.end(), name) == themes.end()) themes.emplace_back(name);
  }
  std::sort(themes.begin(), themes.end(),
            [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
}

void SleepThemeSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
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
  if (selectedIndex == storeIndex()) {
    openStore();
    return;
  }
  openThemeActions(selectedIndex);
}

namespace {
constexpr int ACTION_USE = 0;
constexpr int ACTION_DELETE = 1;
constexpr int ACTION_COUNT = 2;
}  // namespace

void SleepThemeSelectActivity::useTheme(const std::string& name) {
  {
    RenderLock lock(*this);
    strncpy(SETTINGS.sleepTheme, name.c_str(), sizeof(SETTINGS.sleepTheme) - 1);
    SETTINGS.sleepTheme[sizeof(SETTINGS.sleepTheme) - 1] = '\0';
    // Only reachable by an explicit choice, so not a hot write path.
    SETTINGS.saveToFile();
  }
  finish();
}

void SleepThemeSelectActivity::openThemeActions(const int index) {
  // The name is copied into the callbacks: the list is rescanned (and this
  // vector replaced) before a delete finishes.
  const std::string name = themes[static_cast<size_t>(index)];
  // A built-in that isn't on the card has nothing to delete.
  char path[96];
  snprintf(path, sizeof(path), "%s/%s/theme.bin", SleepThemeRenderer::THEMES_DIR, name.c_str());
  if (!Storage.exists(path)) {
    useTheme(name);
    return;
  }
  // Heap: one small activity, gone before this list is back on screen.
  auto picker = makeUniqueNoThrow<ListPickerActivity>(
      renderer, mappedInput, themes[static_cast<size_t>(index)].c_str(), ACTION_COUNT, 0,
      [](int row) { return std::string(row == ACTION_USE ? tr(STR_SLEEP_THEME_USE) : tr(STR_DELETE)); }, nullptr,
      /*markInitial=*/false);
  if (!picker) {
    LOG_ERR("STS", "OOM: theme action list");
    return;
  }
  startActivityForResult(std::move(picker), [this, name](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    if (std::get<ListPickResult>(result.data).index == ACTION_DELETE) {
      confirmDelete(name);
      return;
    }
    useTheme(name);
  });
}

void SleepThemeSelectActivity::confirmDelete(const std::string& name) {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SLEEP_THEME_DELETE_CONFIRM), name),
      [this, name](const ActivityResult& result) {
        if (!result.isCancelled) deleteTheme(name);
        discover();
        placeCursor();
        requestUpdate();
      });
}

void SleepThemeSelectActivity::deleteTheme(const std::string& name) {
  char dir[96];
  snprintf(dir, sizeof(dir), "%s/%s", SleepThemeRenderer::THEMES_DIR, name.c_str());
  if (!Storage.removeDir(dir)) {
    LOG_ERR("STS", "Failed to remove %s", dir);
    return;
  }
  LOG_INF("STS", "Deleted sleep theme %s", name.c_str());
  // The setting must not name a folder that is gone (light and dark carry on
  // as the built-in screen): fall back to light.
  if (strncmp(SETTINGS.sleepTheme, name.c_str(), sizeof(SETTINGS.sleepTheme) - 1) == 0 &&
      !SleepThemeRenderer::isBuiltIn(SETTINGS.sleepTheme)) {
    strncpy(SETTINGS.sleepTheme, "light", sizeof(SETTINGS.sleepTheme) - 1);
    SETTINGS.saveToFile();
  }
}

// The store owns the WiFi lifecycle; this screen only rescans on return, since
// a theme may have been installed or deleted while it was away.
void SleepThemeSelectActivity::openStore() {
  startActivityForResult(std::make_unique<SleepThemeDownloadActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) {
                           discover();
                           placeCursor();
                           requestUpdate();
                         });
}

// With no themes installed, or the configured one missing, the explanation
// takes this many UI_12 lines above the list.
constexpr int EMPTY_MESSAGE_LINES = 4;

// List body between the header and the button hints. Shared by render() and
// the tap hit-testing so the two can never disagree.
Rect SleepThemeSelectActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (themes.empty() || configuredMissing) {
    contentTop += EMPTY_MESSAGE_LINES * renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  }
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void SleepThemeSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SLEEP_THEME));

  if (themes.empty() || configuredMissing) {
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    char message[160];
    if (configuredMissing) {
      snprintf(message, sizeof(message), tr(STR_SLEEP_THEME_MISSING), SETTINGS.sleepTheme);
    } else {
      snprintf(message, sizeof(message), "%s", tr(STR_NO_SLEEP_THEMES));
    }
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, message, pageWidth - 2 * 24, EMPTY_MESSAGE_LINES);
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true);
      y += lineHeight;
    }
  }

  GUI.drawList(
      renderer, listRect(), totalItems(), selectedIndex,
      [this](int index) -> std::string {
        if (index == storeIndex()) return tr(STR_SLEEP_THEME_STORE);
        return themes[static_cast<size_t>(index)];
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (index == storeIndex()) return "";
        const bool active = strncmp(themes[static_cast<size_t>(index)].c_str(), SETTINGS.sleepTheme,
                                    sizeof(SETTINGS.sleepTheme) - 1) == 0;
        return active ? tr(STR_SELECTED) : "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), selectedIndex == storeIndex() ? tr(STR_OPEN) : tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
