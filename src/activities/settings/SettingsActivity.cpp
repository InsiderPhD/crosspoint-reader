#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstring>

#include "BookFusionSettingsActivity.h"
#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DownloadUpdateFromUrlActivity.h"
#include "FontDownloadActivity.h"
#include "FontLayoutPreviewActivity.h"
#include "FontSelectionActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "ManualDateActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "ReaderControlsActivity.h"
#include "RecacheMetadataActivity.h"
#include "RefreshBookFusionMetadataActivity.h"
#include "ResetStatsActivity.h"
#include "SdCardFontGlobals.h"
#include "SdFirmwareUpdateActivity.h"
#include "SettingsList.h"
#include "StatsDataActivity.h"
#include "StatusBarSettingsActivity.h"
#include "TimeZoneSelectActivity.h"
#include "activities/home/LibraryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SettingsActivity::onEnter() {
  Activity::onEnter();

  // Build per-category vectors from the shared settings list
  displaySettings.clear();
  readerSettings.clear();
  statsSettings.clear();
  systemSettings.clear();
  devSettings.clear();

  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    // Device-only override: hide the built-in fontFamily Enum so the FontFamily
    // Action (appended below) is the single picker entry. Web UI still sees the
    // Enum because CrossPointWebServer iterates getSettingsList() directly.
    if (setting.type == SettingType::ENUM && setting.key && std::strcmp(setting.key, "fontFamily") == 0) continue;
    // Device-only override: these reader settings are now edited through the
    // Font & Layout preview (with live sample text), so hide the redundant flat
    // rows from the device Settings list. They remain in the web UI, which
    // iterates getSettingsList() directly.
    if (setting.key &&
        (std::strcmp(setting.key, "fontSize") == 0 || std::strcmp(setting.key, "lineSpacing") == 0 ||
         std::strcmp(setting.key, "paragraphAlignment") == 0 || std::strcmp(setting.key, "screenMargin") == 0 ||
         std::strcmp(setting.key, "extraParagraphSpacing") == 0 || std::strcmp(setting.key, "bionicReading") == 0 ||
         std::strcmp(setting.key, "textAntiAliasing") == 0 || std::strcmp(setting.key, "orientation") == 0)) {
      continue;
    }
    // Device-only override: the "if found" contact is a free-text field with no
    // on-device keyboard entry path, so it is edited from the web UI only. The
    // web UI iterates getSettingsList() directly, so it still sees this entry.
    if (setting.key && std::strcmp(setting.key, "returnContact") == 0) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_STATS) {
      statsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append device-only ACTION items — reader controls actions go first in System
  systemSettings.insert(systemSettings.begin(),
                        SettingInfo::Action(StrId::STR_READER_CONTROLS, SettingAction::ReaderControls));
  systemSettings.insert(systemSettings.begin(),
                        SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_BF_SYNC, SettingAction::BookFusionSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  // Stats tab device-only actions (date/timezone pickers). The daily goal +
  // min-session-length Enum entries are already populated above from
  // SettingsList — these actions go below them.
  statsSettings.push_back(SettingInfo::Action(StrId::STR_SET_DATE, SettingAction::SetDate));
  statsSettings.push_back(SettingInfo::Action(StrId::STR_TIME_ZONE, SettingAction::TimeZone));
  // Data management: lossless JSON export/import (round-trip backup) plus a
  // one-way StoryGraph-compatible CSV of the book catalog.
  statsSettings.push_back(SettingInfo::Action(StrId::STR_EXPORT_READING_STATS, SettingAction::ExportStats));
  statsSettings.push_back(SettingInfo::Action(StrId::STR_IMPORT_READING_STATS, SettingAction::ImportStats));
  statsSettings.push_back(SettingInfo::Action(StrId::STR_EXPORT_STORYGRAPH, SettingAction::ExportStoryGraph));

  // --- Dev tools ---
  // These are all testing aids / risky operations, collected under a dedicated
  // Dev tab that is only shown when Dev Mode is enabled (see category assembly
  // below). Dev Mode itself is toggled from the System tab.
  // Clear Reading Cache: testing aid.
  devSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  // Recache Library: forces a full SD re-scan on the next library open. Safety
  // net for the rare case the library index misses a file added outside the
  // normal download/upload paths (its dir-mtime validation can't see SD-root
  // additions on FAT).
  devSettings.push_back(SettingInfo::Action(StrId::STR_RECACHE_LIBRARY, SettingAction::RecacheLibrary));
  // Recache Metadata: builds book.bin for any book that lacks it, so tags (and
  // other metadata) exist for books that have never been opened. Needed to make
  // Tag-folder mode complete. Slow (full parse per uncached book).
  devSettings.push_back(SettingInfo::Action(StrId::STR_RECACHE_METADATA, SettingAction::RecacheMetadata));
  // Refresh BookFusion Data: re-downloads API-sourced metadata (covers,
  // categories/shelves/lists, reading position) for every locally-downloaded
  // BookFusion book. Needs WiFi.
  devSettings.push_back(SettingInfo::Action(StrId::STR_REFRESH_BF_METADATA, SettingAction::RefreshBookFusionMetadata));
  // Download Update from URL: raw-writes firmware (bypasses image verify) — bricking
  // risk for casual users.
  devSettings.push_back(SettingInfo::Action(StrId::STR_DOWNLOAD_FROM_URL, SettingAction::DownloadFromUrl));
  // Reset Reading Stats: testing aid.
  devSettings.push_back(SettingInfo::Action(StrId::STR_RESET_READING_STATS, SettingAction::ResetStats));

  readerSettings.push_back(SettingInfo::Action(StrId::STR_FONT_LAYOUT_PREVIEW, SettingAction::FontLayoutPreview));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));
  // Font family (built-in + SD) and Font Download are now edited inside the
  // Font & Layout preview, so they are not listed separately here.

  // Assemble the visible category tabs. categoryNames and categoryLists stay in
  // lockstep. The Dev tab is only appended when Dev Mode is enabled.
  categoryNames = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER, StrId::STR_CAT_STATS, StrId::STR_CAT_SYSTEM};
  categoryLists = {&displaySettings, &readerSettings, &statsSettings, &systemSettings};
  if (SETTINGS.devMode) {
    categoryNames.push_back(StrId::STR_CAT_DEV);
    categoryLists.push_back(&devSettings);
  }
  categoryCount = static_cast<int>(categoryNames.size());

  // Reset selection to first category
  selectedCategoryIndex = 0;
  selectedSettingIndex = 0;

  // Initialize with first category (Display)
  currentSettings = categoryLists[0];
  settingsCount = static_cast<int>(currentSettings->size());

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  bool hasChangedCategory = false;

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      selectedCategoryIndex = (selectedCategoryIndex < categoryCount - 1) ? (selectedCategoryIndex + 1) : 0;
      hasChangedCategory = true;
      requestUpdate();
    } else {
      toggleCurrentSetting();
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      onGoHome();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, &hasChangedCategory] {
    hasChangedCategory = true;
    selectedCategoryIndex = ButtonNavigator::previousIndex(selectedCategoryIndex, categoryCount);
    requestUpdate();
  });

  if (hasChangedCategory) {
    selectedSettingIndex = (selectedSettingIndex == 0) ? 0 : 1;
    currentSettings = categoryLists[selectedCategoryIndex];
    settingsCount = static_cast<int>(currentSettings->size());
  }
}

void SettingsActivity::toggleCurrentSetting() {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
  } else if (setting.type == SettingType::ENUM) {
    if (setting.valuePtr != nullptr) {
      const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
      SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    } else if (setting.valueGetter && setting.valueSetter) {
      const uint8_t currentValue = setting.valueGetter();
      setting.valueSetter((currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size()));
    } else {
      return;
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    const int8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderControls:
        startActivityForResult(std::make_unique<ReaderControlsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::KOReaderSync:
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::BookFusionSync:
        startActivityForResult(std::make_unique<BookFusionSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::OPDSBrowser:
        startActivityForResult(std::make_unique<CalibreSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RecacheLibrary:
        // Non-destructive: drop the library index so the next library open does a
        // full SD re-scan. Confirm first (matches every other action launching an
        // activity); on confirm, invalidate.
        startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_RECACHE_LIBRARY),
                                                                      tr(STR_LIBRARY_RECACHED)),
                               [this](const ActivityResult& res) {
                                 if (!res.isCancelled) LibraryActivity::invalidateIndexCache();
                                 SETTINGS.saveToFile();
                               });
        break;
      case SettingAction::RecacheMetadata:
        startActivityForResult(std::make_unique<RecacheMetadataActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::RefreshBookFusionMetadata:
        startActivityForResult(std::make_unique<RefreshBookFusionMetadataActivity>(renderer, mappedInput),
                               resultHandler);
        break;
      case SettingAction::CheckForUpdates:
        startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFromUrl:
        startActivityForResult(std::make_unique<DownloadUpdateFromUrlActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ResetStats:
        startActivityForResult(std::make_unique<ResetStatsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ExportStats:
        startActivityForResult(std::make_unique<StatsDataActivity>(renderer, mappedInput, StatsDataMode::ExportJson),
                               resultHandler);
        break;
      case SettingAction::ImportStats:
        startActivityForResult(std::make_unique<StatsDataActivity>(renderer, mappedInput, StatsDataMode::ImportJson),
                               resultHandler);
        break;
      case SettingAction::ExportStoryGraph:
        startActivityForResult(
            std::make_unique<StatsDataActivity>(renderer, mappedInput, StatsDataMode::ExportStoryGraph), resultHandler);
        break;
      case SettingAction::SetDate:
        startActivityForResult(std::make_unique<ManualDateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::TimeZone:
        startActivityForResult(std::make_unique<TimeZoneSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::FontFamily:
        startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 ensureSdFontLoaded();
                               });
        break;
      case SettingAction::FontDownload:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { ensureSdFontLoaded(); });
        break;
      case SettingAction::FontLayoutPreview:
        // The preview persists its own settings and restores the renderer to
        // Portrait on exit; just reload the (possibly changed) active font.
        startActivityForResult(std::make_unique<FontLayoutPreviewActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { ensureSdFontLoaded(); });
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    return;  // Results will be handled in the result handler, so we can return early here
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                 selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  GUI.drawList(
      renderer,
      Rect{0, metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing, pageWidth,
           pageHeight - (metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.buttonHintsHeight +
                         metrics.verticalSpacing * 2)},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM) {
          uint8_t value = 0;
          if (setting.valuePtr != nullptr) {
            value = SETTINGS.*(setting.valuePtr);
          } else if (setting.valueGetter) {
            value = setting.valueGetter();
          }
          if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          valueText = std::to_string(SETTINGS.*(setting.valuePtr));
        } else if (setting.type == SettingType::ACTION && setting.action == SettingAction::FontFamily) {
          if (SETTINGS.sdFontFamilyName[0] != '\0') {
            valueText = SETTINGS.sdFontFamilyName;
          } else {
            static const StrId builtinFontNames[] = {StrId::STR_BOOKERLY, StrId::STR_INTER, StrId::STR_OPEN_DYSLEXIC,
                                                     StrId::STR_MONOSPACE};
            const uint8_t f = SETTINGS.fontFamily;
            if (f < sizeof(builtinFontNames) / sizeof(builtinFontNames[0])) {
              valueText = I18N.get(builtinFontNames[f]);
            }
          }
        }
        return valueText;
      },
      true);

  // Draw help text
  const auto confirmLabel = (selectedSettingIndex == 0)
                                ? I18N.get(categoryNames[(selectedCategoryIndex + 1) % categoryCount])
                                : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Always use standard refresh for settings screen
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
