#include "RefreshBookFusionMetadataActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "BookFusionMetaStore.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/home/LibraryScan.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionCoverCache.h"

void RefreshBookFusionMetadataActivity::onEnter() {
  Activity::onEnter();
  state = WARNING;
  requestUpdate();
}

void RefreshBookFusionMetadataActivity::onExit() {
  Activity::onExit();
  // Mirror BookFusionSyncActivity: drop WiFi so the charge pump / radio aren't
  // left powered after the activity closes.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void RefreshBookFusionMetadataActivity::startRun() {
  {
    RenderLock lock(*this);
    state = RUNNING;
  }
  requestUpdateAndWait();  // Paint the "Refreshing…" screen before the blocking pass.
  refreshAll();
}

void RefreshBookFusionMetadataActivity::onWifiComplete(bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_WIFI_CONN_FAILED), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }
  startRun();
}

void RefreshBookFusionMetadataActivity::refreshOneBook(const BookFusionBook& book, const std::string& path,
                                                       int coverHeight) {
  Epub epub(path, "/.crosspoint");
  bool anyFail = false;

  // Cover: re-download and re-convert. A book with no API cover URL isn't a
  // failure — there's simply nothing to refresh.
  if (book.coverUrl[0] != '\0') {
    if (BookFusionCoverCache::refresh(book.coverUrl, epub, coverHeight)) {
      coversOk++;
    } else {
      anyFail = true;
    }
  }

  // Organisational metadata (categories / bookshelves / lists). Saving an empty
  // meta just clears a stale sidecar, which is still a valid refresh outcome.
  BookFusionMeta meta;
  meta.categories = book.categories;
  meta.bookshelves = book.bookshelves;
  meta.lists = book.lists;
  if (BookFusionMetaStore::save(epub.getCachePath(), meta)) {
    metaOk++;
  } else {
    anyFail = true;
  }

  // Reading position — ONLY when there's no local progress yet, so we never
  // clobber the position of a book being read on this device. BookFusion's
  // book-level position is coarser (chapter granularity) and offline we can't
  // reliably tell whether the remote is ahead of local, so overwriting would
  // risk silently losing local reading progress.
  const std::string progressPath = epub.getCachePath() + "/progress.bin";
  if (!Storage.exists(progressPath.c_str())) {
    BookFusionPosition remotePos{};
    const auto syncErr = BookFusionSyncClient::getProgress(book.id, remotePos);
    if (syncErr == BookFusionSyncClient::OK && remotePos.percentage > 0.0f) {
      // Validate the remote chapter index against the spine before writing.
      // book.bin usually already exists (downloaded / recached), so this load is
      // cheap; buildIfMissing=false avoids a full parse when it doesn't.
      if (epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
        const int spineCount = epub.getSpineItemsCount();
        if (remotePos.chapterIndex >= 0 && remotePos.chapterIndex < spineCount) {
          FsFile progressFile;
          if (Storage.openFileForWrite("BFR", progressPath, progressFile)) {
            // progress.bin format (EpubReaderActivity.cpp): [0..1] spineIndex LE,
            // [2..3] pageNumber LE (0 = start of chapter).
            const uint16_t spineIndex = static_cast<uint16_t>(remotePos.chapterIndex);
            uint8_t data[4] = {static_cast<uint8_t>(spineIndex & 0xFF), static_cast<uint8_t>((spineIndex >> 8) & 0xFF),
                               0, 0};
            progressFile.write(data, sizeof(data));
            progressFile.flush();
            progressFile.close();
            if (remotePos.updatedAt[0] != '\0') {
              BookFusionBookIdStore::saveLastSyncAt(path.c_str(), remotePos.updatedAt);
            }
            // Surface BookFusion's authoritative book-level percentage in the
            // library row without waiting for the reader to render a page.
            const auto progressPercent = static_cast<int8_t>(std::clamp(remotePos.percentage, 0.0f, 100.0f) + 0.5f);
            RECENT_BOOKS.updateProgress(path, progressPercent);
            positionsOk++;
          }
        }
      }
    }
  }

  if (anyFail) failed++;
}

void RefreshBookFusionMetadataActivity::refreshAll() {
  LOG_DBG("BFR", "Refreshing BookFusion metadata...");

  // 1. Enumerate local books that came from BookFusion (sidecar present).
  std::vector<std::string> bookPaths;
  LibraryScan::enumerateBooks(bookPaths);

  struct LocalBook {
    uint32_t id;
    std::string path;
    bool done;
  };
  std::vector<LocalBook> localBooks;
  localBooks.reserve(bookPaths.size());
  for (const auto& path : bookPaths) {
    if (!FsHelpers::hasEpubExtension(path)) continue;
    const uint32_t id = BookFusionBookIdStore::loadBookId(path.c_str());
    if (id != 0) localBooks.push_back({id, path, false});
  }

  totalLocal = static_cast<int>(localBooks.size());
  matched = coversOk = metaOk = positionsOk = failed = 0;

  if (localBooks.empty()) {
    LOG_DBG("BFR", "No BookFusion books found locally");
    RenderLock lock(*this);
    state = SUCCESS;
    requestUpdate();
    return;
  }

  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;

  // 2. Walk the remote library (ALL_BOOKS) page by page and match returned books
  //    against our local set by book_id. There is no get-by-id endpoint, so the
  //    paginated search is the only source of cover URL + categories/shelves/lists.
  //    Stop as soon as every local book has been matched.
  constexpr int kMaxPages = 500;  // Safety cap: bounds the walk for very large libraries.
  int page = 1;
  int remaining = totalLocal;
  bool networkError = false;

  while (page <= kMaxPages && remaining > 0) {
    BookFusionSearchResult result;
    const auto err = BookFusionSyncClient::searchBooks(page, result, nullptr, nullptr, 0);
    if (err != BookFusionSyncClient::OK) {
      // Fail hard only if the very first page failed (nothing done yet); a
      // later-page failure just ends the walk with whatever we managed.
      if (page == 1) {
        networkError = true;
        strlcpy(errorMsg, BookFusionSyncClient::errorString(err), sizeof(errorMsg));
      }
      break;
    }
    if (result.count == 0) break;

    for (int i = 0; i < result.count && remaining > 0; ++i) {
      const BookFusionBook& book = result.books[i];
      LocalBook* local = nullptr;
      for (auto& lb : localBooks) {
        if (!lb.done && lb.id == book.id) {
          local = &lb;
          break;
        }
      }
      if (local == nullptr) continue;

      refreshOneBook(book, local->path, coverHeight);
      local->done = true;
      matched++;
      remaining--;
      // Cover conversion + the reading-position EPUB load are slow; yield so the
      // walk can't starve the task watchdog.
      vTaskDelay(1);
    }

    if (!result.hasMore) break;
    page++;
  }

  if (networkError) {
    RenderLock lock(*this);
    state = ERROR;
    requestUpdate();
    return;
  }

  // Covers / progress may have changed; drop the index so the library re-reads.
  LibraryScan::invalidateIndex();

  LOG_DBG("BFR", "BookFusion refresh done: matched %d/%d, covers %d, meta %d, positions %d, failed %d", matched,
          totalLocal, coversOk, metaOk, positionsOk, failed);

  RenderLock lock(*this);
  state = SUCCESS;
  requestUpdate();
}

void RefreshBookFusionMetadataActivity::loop() {
  if (state == WIFI_SELECTION || state == RUNNING) return;

  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!BF_TOKEN_STORE.hasToken()) {
        {
          RenderLock lock(*this);
          state = ERROR;
          strlcpy(errorMsg, tr(STR_BF_NO_TOKEN_MSG), sizeof(errorMsg));
        }
        requestUpdate();
        return;
      }
      if (WiFi.status() == WL_CONNECTED) {
        startRun();
      } else {
        {
          RenderLock lock(*this);
          state = WIFI_SELECTION;
        }
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  // SUCCESS or ERROR — Back closes.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
  }
}

void RefreshBookFusionMetadataActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REFRESH_BF_METADATA));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_REFRESH_BF_METADATA_WARNING), true);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WIFI_SELECTION || state == RUNNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_REFRESHING_BF_METADATA));
  } else if (state == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, errorMsg, true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {  // SUCCESS
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_BF_METADATA_REFRESHED), true,
                              EpdFontFamily::BOLD);
    std::string resultText = std::to_string(matched) + " / " + std::to_string(totalLocal);
    if (failed > 0) {
      resultText += ", " + std::to_string(failed) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 6, resultText.c_str());

    // Second line: what actually got refreshed. Format string is translatable
    // (two %d: covers, positions) — mirrors STR_BF_TIME_REMAINING's pattern.
    char detail[64];
    snprintf(detail, sizeof(detail), tr(STR_BF_REFRESH_DETAIL), coversOk, positionsOk);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 30, detail);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
