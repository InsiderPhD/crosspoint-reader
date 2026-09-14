#include "BookFusionCoverRefreshActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/home/LibraryScan.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionCoverCache.h"

namespace {
// A title query normally lands the book on page 1; a couple more pages cover
// common-word titles. The fallback walk is capped like the bulk refresh.
constexpr int kMaxQueryPages = 3;
constexpr int kMaxWalkPages = 500;
constexpr int kMaxCoverAttempts = 2;
}  // namespace

void BookFusionCoverRefreshActivity::onEnter() {
  Activity::onEnter();
  state = STARTING;
  requestUpdate();
}

void BookFusionCoverRefreshActivity::onExit() {
  Activity::onExit();
  // Mirror RefreshBookFusionMetadataActivity: don't leave the radio powered.
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void BookFusionCoverRefreshActivity::fail(const char* message) {
  {
    RenderLock lock(*this);
    state = ERROR;
    strlcpy(errorMsg, message, sizeof(errorMsg));
  }
  requestUpdate();
}

void BookFusionCoverRefreshActivity::begin() {
  if (!BF_TOKEN_STORE.hasToken()) {
    fail(tr(STR_BF_NO_TOKEN_MSG));
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    run();
    return;
  }
  {
    RenderLock lock(*this);
    state = WIFI_SELECTION;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void BookFusionCoverRefreshActivity::onWifiComplete(const bool success) {
  if (!success) {
    fail(tr(STR_WIFI_CONN_FAILED));
    return;
  }
  run();
}

bool BookFusionCoverRefreshActivity::findCoverUrl(const uint32_t bookId, char* outUrl, const size_t outUrlLen) {
  // One borrow for the whole run of searches: they share the kept-alive API
  // connection and nothing renders in between ("Downloading cover..." is
  // already on the panel).
  TlsFramebufferBorrow borrow(renderer);

  // Pass 0 queries by title; pass 1 walks the whole library. A title that
  // isn't known (or matches nothing) goes straight to the walk.
  for (int pass = title.empty() ? 1 : 0; pass < 2; ++pass) {
    const char* query = pass == 0 ? title.c_str() : nullptr;
    const int maxPages = pass == 0 ? kMaxQueryPages : kMaxWalkPages;
    for (int page = 1; page <= maxPages; ++page) {
      const auto err = BookFusionSyncClient::searchBooks(page, searchResult, nullptr, nullptr, 0, query);
      if (err != BookFusionSyncClient::OK) {
        LOG_ERR("BFCR", "searchBooks pass=%d page=%d failed: %d", pass, page, static_cast<int>(err));
        strlcpy(errorMsg, BookFusionSyncClient::errorString(err), sizeof(errorMsg));
        return false;
      }
      for (int i = 0; i < searchResult.count; ++i) {
        if (searchResult.books[i].id != bookId) continue;
        if (searchResult.books[i].coverUrl[0] == '\0') {
          LOG_ERR("BFCR", "book_id=%lu has no API cover URL", static_cast<unsigned long>(bookId));
          strlcpy(errorMsg, tr(STR_BF_COVER_FAILED), sizeof(errorMsg));
          return false;
        }
        strlcpy(outUrl, searchResult.books[i].coverUrl, outUrlLen);
        LOG_DBG("BFCR", "Found book_id=%lu on pass=%d page=%d", static_cast<unsigned long>(bookId), pass, page);
        return true;
      }
      if (!searchResult.hasMore || searchResult.count == 0) break;
      vTaskDelay(1);  // A long walk must not starve the task watchdog.
    }
  }

  LOG_ERR("BFCR", "book_id=%lu not found in the BookFusion library", static_cast<unsigned long>(bookId));
  strlcpy(errorMsg, tr(STR_BF_COVER_NOT_FOUND), sizeof(errorMsg));
  return false;
}

void BookFusionCoverRefreshActivity::run() {
  {
    RenderLock lock(*this);
    state = RUNNING;
  }
  requestUpdateAndWait();  // Paint "Downloading cover..." before the blocking pass.

  const uint32_t bookId = BookFusionBookIdStore::loadBookId(path.c_str());
  if (bookId == 0) {
    LOG_ERR("BFCR", "No BookFusion book_id for %s", path.c_str());
    fail(tr(STR_BF_COVER_NOT_FOUND));
    return;
  }

  // book.bin supplies the real title for the search query and the author for
  // the recents entry; buildIfMissing=false keeps this cheap.
  Epub epub(path, "/.crosspoint");
  const bool loaded = epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true);
  if (loaded && !epub.getTitle().empty()) title = epub.getTitle();
  epub.setupCacheDir();

  if (!findCoverUrl(bookId, coverUrl, sizeof(coverUrl))) {
    {
      RenderLock lock(*this);
      state = ERROR;
    }
    requestUpdate();
    return;
  }

  // Same two-phase lease dance as BookFusionBrowserActivity::startDownload:
  // borrow the framebuffer for the TLS fetch, then lease it to the JPEG decoder
  // for the conversions. The leases never overlap. Download first, so a failed
  // fetch leaves the existing thumbnails untouched.
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  char thumbPath[128] = {};
  bool ok = false;
  for (int attempt = 1; attempt <= kMaxCoverAttempts && !ok; ++attempt) {
    bool fetched = false;
    {
      TlsFramebufferBorrow borrow(renderer);
      fetched = BookFusionCoverCache::download(coverUrl, epub, BookFusionCoverCache::kCoverFetchWidth,
                                               BookFusionCoverCache::kCoverFetchHeight);
    }
    if (!fetched) {
      LOG_ERR("BFCR", "Cover fetch attempt %d/%d failed", attempt, kMaxCoverAttempts);
      continue;
    }
    RenderLock lock(*this);
    JpegScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
    ok = BookFusionCoverCache::convert(epub, coverHeight, thumbPath, sizeof(thumbPath));
    if (!ok) LOG_ERR("BFCR", "Cover convert attempt %d/%d failed", attempt, kMaxCoverAttempts);
  }

  if (!ok) {
    fail(tr(STR_BF_COVER_FAILED));
    return;
  }

  if (loaded) RECENT_BOOKS.updateBook(path, epub.getTitle(), epub.getAuthor(), thumbPath);
  LibraryScan::invalidateIndex();
  LOG_DBG("BFCR", "BookFusion cover regenerated: %s", thumbPath);

  {
    RenderLock lock(*this);
    state = SUCCESS;
  }
  requestUpdate();
}

void BookFusionCoverRefreshActivity::loop() {
  switch (state) {
    case STARTING:
      begin();
      return;
    case WIFI_SELECTION:
    case RUNNING:
      return;
    case SUCCESS:
    case ERROR:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        finish();
      }
      return;
  }
}

void BookFusionCoverRefreshActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REGENERATE_COVER));

  switch (state) {
    case STARTING:
    case WIFI_SELECTION:
    case RUNNING:
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_BF_COVER_DOWNLOADING));
      break;
    case SUCCESS:
    case ERROR: {
      const char* message = state == SUCCESS ? tr(STR_BF_COVER_UPDATED) : errorMsg;
      const int contentWidth = pageWidth - metrics.contentSidePadding * 2;
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, message, contentWidth, 4, EpdFontFamily::BOLD);
      int y = pageHeight / 2 - (static_cast<int>(lines.size()) * lineHeight) / 2;
      for (const auto& line : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
        y += lineHeight;
      }
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
