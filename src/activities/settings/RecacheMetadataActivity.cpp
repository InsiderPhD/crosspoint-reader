#include "RecacheMetadataActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/LibraryScan.h"
#include "components/UITheme.h"
#include "fontIds.h"

void RecacheMetadataActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void RecacheMetadataActivity::onExit() { Activity::onExit(); }

void RecacheMetadataActivity::recacheMetadata() {
  LOG_DBG("RECACHE", "Re-caching metadata...");

  std::vector<std::string> bookPaths;
  LibraryScan::enumerateBooks(bookPaths);

  totalBooks = static_cast<int>(bookPaths.size());
  processed = 0;
  builtCount = 0;
  failedCount = 0;
  requestUpdateAndWait();  // Repaint now that totalBooks is known ("0 / N").

  // Bound the number of progress repaints so a large library doesn't pay an e-ink
  // refresh per book: at most ~40 updates regardless of size.
  const int updateStep = (totalBooks > 40) ? (totalBooks / 40) : 1;

  for (const auto& path : bookPaths) {
    // Force a metadata rebuild so tags/author get (re-)extracted with the current
    // parser — including books whose book.bin already exists but predates or lacks
    // that metadata. Only book.bin is removed; the pre-rendered sections/ and cover
    // are left intact, so this is non-destructive to reading progress or layout.
    if (FsHelpers::hasEpubExtension(path)) {
      Epub epub(path, "/.crosspoint");
      const std::string bookBin = epub.getCachePath() + "/book.bin";
      if (Storage.exists(bookBin.c_str())) Storage.remove(bookBin.c_str());
      if (epub.load(/*buildIfMissing=*/true, /*skipLoadingCss=*/true)) {
        builtCount++;
      } else {
        failedCount++;
        LOG_ERR("RECACHE", "Failed to build metadata: %s", path.c_str());
      }
    }
    processed++;
    // Yield each iteration: an OPF/TOC re-parse can be slow and the loop can run for
    // hundreds of books — must not starve the task watchdog.
    vTaskDelay(1);
    // Repaint the "N / total" counter so the screen visibly advances (fast refresh).
    if (processed == totalBooks || (processed % updateStep) == 0) requestUpdateAndWait();
  }

  // New book.bins mean the tag view (and library) should re-read; drop the index.
  LibraryScan::invalidateIndex();

  LOG_DBG("RECACHE", "Metadata recache done: %d built, %d failed of %d", builtCount, failedCount, totalBooks);
  state = SUCCESS;
  requestUpdate();
}

void RecacheMetadataActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = RUNNING;
      }
      requestUpdateAndWait();  // Paint the "Re-caching…" screen before the blocking pass.
      recacheMetadata();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}

void RecacheMetadataActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_RECACHE_METADATA));

  // RUNNING repaints per book, so use FAST refresh there; the static WARNING/SUCCESS
  // screens use a FULL refresh (SUCCESS also clears any ghosting from the fast updates).
  HalDisplay::RefreshMode refreshMode = HalDisplay::FULL_REFRESH;

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_RECACHE_METADATA_WARNING), true);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == RUNNING) {
    refreshMode = HalDisplay::FAST_REFRESH;
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_RECACHING_METADATA));
    // Show live progress once the book count is known.
    if (totalBooks > 0) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d / %d", processed, totalBooks);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 15, buf, true, EpdFontFamily::BOLD);
    }
  } else {  // SUCCESS
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_METADATA_RECACHED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(builtCount) + " / " + std::to_string(totalBooks);
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(refreshMode);
}
