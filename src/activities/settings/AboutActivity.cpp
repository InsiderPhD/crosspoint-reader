#include "AboutActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TouchListNav.h"

namespace {
enum Row {
  ROW_EXPORT = 0,
  ROW_FIRMWARE,
  ROW_DEVICE,
  ROW_CHIP,
  ROW_FLASH,
  ROW_DISPLAY,
  ROW_BATTERY,
  ROW_FREE_MEMORY,
  ROW_LARGEST_BLOCK,
  ROW_MIN_FREE,
  ROW_PSRAM,
  ROW_SD_CARD,
  ROW_SD_FREE,
  ROW_TOUCH,
  ROW_FRONTLIGHT,
  ROW_RTC,
  ROW_TILT,
  ROW_MAC,
  ROW_UPTIME,
  ROW_RESET,
  ROW_COUNT
};

constexpr StrId rowNames[ROW_COUNT] = {
    StrId::STR_ABOUT_EXPORT,   StrId::STR_ABOUT_FIRMWARE,    StrId::STR_DEVICE_NAME,
    StrId::STR_ABOUT_CHIP,     StrId::STR_ABOUT_FLASH,       StrId::STR_ABOUT_DISPLAY,
    StrId::STR_BATTERY,        StrId::STR_ABOUT_FREE_MEMORY, StrId::STR_ABOUT_LARGEST_BLOCK,
    StrId::STR_ABOUT_MIN_FREE, StrId::STR_ABOUT_PSRAM,       StrId::STR_SD_CARD,
    StrId::STR_ABOUT_SD_FREE,  StrId::STR_ABOUT_TOUCH,       StrId::STR_ABOUT_FRONTLIGHT,
    StrId::STR_ABOUT_RTC,      StrId::STR_ABOUT_TILT,        StrId::STR_ABOUT_MAC,
    StrId::STR_ABOUT_UPTIME,   StrId::STR_ABOUT_RESET,
};

// Longest value is the MAC ("AA:BB:CC:DD:EE:FF") or a chip line; 40 is ample.
constexpr size_t VALUE_BUFFER_SIZE = 40;

// "512 KB" / "16 MB" / "29.7 GB". Unit suffixes are SI-style identifiers, not prose.
void formatBytes(const uint64_t bytes, char* buf, const size_t bufSize) {
  constexpr uint64_t KB = 1024ULL;
  constexpr uint64_t MB = KB * 1024ULL;
  constexpr uint64_t GB = MB * 1024ULL;
  if (bytes >= GB) {
    // Tenths of a GB in integer maths: this board has no FPU worth waking for it.
    const uint64_t tenths = (bytes * 10ULL) / GB;
    snprintf(buf, bufSize, "%u.%u GB", static_cast<unsigned>(tenths / 10ULL), static_cast<unsigned>(tenths % 10ULL));
  } else if (bytes >= MB) {
    snprintf(buf, bufSize, "%u MB", static_cast<unsigned>(bytes / MB));
  } else {
    snprintf(buf, bufSize, "%u KB", static_cast<unsigned>(bytes / KB));
  }
}

const char* presentText(const bool present) { return present ? tr(STR_ABOUT_PRESENT) : tr(STR_ABOUT_NOT_PRESENT); }
}  // namespace

void AboutActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  exportState = ExportState::Idle;
  snapshot = SupportInfo::capture(mappedInput.hasTouch());
  // No card (or the simulator): there is no usage to work out.
  if (snapshot.sdTotalBytes == 0) snapshot.sdUsageKnown = true;
  requestUpdate();
}

void AboutActivity::exportSupportInfo() {
  // SD work happens here, on the loop task, outside the render lock.
  const bool saved = SupportInfo::exportToSd(snapshot);
  {
    RenderLock lock(*this);
    exportState = saved ? ExportState::Saved : ExportState::Failed;
    selectedIndex = ROW_EXPORT;  // bring the result row into view
  }
  requestUpdate();
}

void AboutActivity::loop() {
  if (!snapshot.sdUsageKnown) {
    // SD usage means walking the card's allocation table (seconds on a big
    // card). Draw the screen first, then fill the row in.
    requestUpdateAndWait();
    SupportInfo::Snapshot updated = snapshot;
    SupportInfo::captureSdUsage(updated);
    {
      RenderLock lock(*this);
      snapshot = updated;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), ROW_COUNT, selectedIndex, /*hasSubtitle=*/false, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedIndex = tappedIndex;
      // Only the export row does anything when tapped; a second tap on an
      // information row must not write a file the user did not ask for.
      if (tappedIndex == ROW_EXPORT) {
        exportSupportInfo();
      } else {
        requestUpdate();
      }
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    exportSupportInfo();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  // Full Touch: a vertical swipe turns a page, matching the held side key.
  const int pageItems = GUI.listGeometry(listRect(), 0, /*hasSubtitle=*/false).pageItems;
  if (TouchListNav::pageSwipe(mappedInput, ROW_COUNT, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });
}

std::string AboutActivity::valueText(const int row) const {
  const SupportInfo::Snapshot& s = snapshot;
  char buf[VALUE_BUFFER_SIZE];
  buf[0] = '\0';
  switch (row) {
    case ROW_EXPORT:
      if (exportState == ExportState::Saved) return tr(STR_ABOUT_EXPORT_SAVED);
      if (exportState == ExportState::Failed) return tr(STR_EXPORT_FAILED);
      return std::string();
    case ROW_FIRMWARE:
      return CROSSPOINT_VERSION;
    case ROW_DEVICE:
      return s.boardName;
    case ROW_CHIP:
      if (s.cpuMHz > 0) {
        snprintf(buf, sizeof(buf), "%s r%u, %u MHz", s.chipModel, static_cast<unsigned>(s.chipRevision),
                 static_cast<unsigned>(s.cpuMHz));
      } else {
        snprintf(buf, sizeof(buf), "%s", s.chipModel);
      }
      break;
    case ROW_FLASH:
      if (s.flashBytes == 0) return tr(STR_UNKNOWN);
      formatBytes(s.flashBytes, buf, sizeof(buf));
      break;
    case ROW_DISPLAY:
      if (s.displayWidth > 0) {
        snprintf(buf, sizeof(buf), "%s %ux%u", s.displayController, static_cast<unsigned>(s.displayWidth),
                 static_cast<unsigned>(s.displayHeight));
      } else {
        snprintf(buf, sizeof(buf), "%s", s.displayController);
      }
      break;
    case ROW_BATTERY:
      snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(s.batteryPercent));
      break;
    case ROW_FREE_MEMORY:
      formatBytes(s.heapFree, buf, sizeof(buf));
      break;
    case ROW_LARGEST_BLOCK:
      formatBytes(s.heapLargestBlock, buf, sizeof(buf));
      break;
    case ROW_MIN_FREE:
      formatBytes(s.heapMinFree, buf, sizeof(buf));
      break;
    case ROW_PSRAM:
      if (s.psramBytes == 0) return tr(STR_ABOUT_NOT_PRESENT);
      formatBytes(s.psramBytes, buf, sizeof(buf));
      break;
    case ROW_SD_CARD:
      if (s.sdTotalBytes == 0) return tr(STR_UNKNOWN);
      formatBytes(s.sdTotalBytes, buf, sizeof(buf));
      break;
    case ROW_SD_FREE:
      if (!s.sdUsageKnown) return tr(STR_ABOUT_CHECKING);
      if (s.sdTotalBytes == 0) return tr(STR_UNKNOWN);
      formatBytes(s.sdTotalBytes > s.sdUsedBytes ? s.sdTotalBytes - s.sdUsedBytes : 0, buf, sizeof(buf));
      break;
    case ROW_TOUCH:
      return presentText(s.hasTouch);
    case ROW_FRONTLIGHT:
      return presentText(s.hasFrontlight);
    case ROW_RTC:
      return presentText(s.hasRtc);
    case ROW_TILT:
      return presentText(s.hasTiltSensor);
    case ROW_MAC:
      if (!s.macValid) return tr(STR_UNKNOWN);
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4],
               s.mac[5]);
      break;
    case ROW_UPTIME: {
      const unsigned long minutes = s.uptimeSeconds / 60UL;
      // Same compact h/m style as the status bar's time-left readout.
      snprintf(buf, sizeof(buf), "%luh %02lum", minutes / 60UL, minutes % 60UL);
      break;
    }
    case ROW_RESET:
      snprintf(buf, sizeof(buf), "%s (%u)", s.resetReason, static_cast<unsigned>(s.resetReasonCode));
      break;
    default:
      break;
  }
  return buf;
}

// List body between the header and the button hints. Shared by render() and
// the loop()'s tap hit-testing so the two can never disagree.
Rect AboutActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void AboutActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ABOUT));

  GUI.drawList(
      renderer, listRect(), ROW_COUNT, selectedIndex, [](int index) { return std::string(I18N.get(rowNames[index])); },
      nullptr, nullptr, [this](int index) { return valueText(index); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ABOUT_EXPORT_SHORT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
