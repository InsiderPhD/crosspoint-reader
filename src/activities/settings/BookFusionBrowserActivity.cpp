#include "BookFusionBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "BookFusionBookIdStore.h"
#include "BookFusionMetaStore.h"
#include "BookFusionTokenStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/home/LibraryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionCoverCache.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include "util/TouchListNav.h"

namespace {
struct Category {
  StrId nameId;
  const char* list;
  const char* sort;
  UIIcon icon;
};

constexpr Category CATEGORIES[] = {
    {StrId::STR_BF_CURRENTLY_READING, "currently_reading", "last_read_at-desc", UIIcon::Book},
    {StrId::STR_BF_FAVORITES, "favorites", nullptr, UIIcon::Star},
    {StrId::STR_BF_PLAN_TO_READ, "planned_to_read", nullptr, UIIcon::Arrow},
    {StrId::STR_BF_COMPLETED, "completed", nullptr, UIIcon::Check},
    {StrId::STR_BF_ALL_BOOKS, nullptr, nullptr, UIIcon::Files},
};
constexpr int NUM_CATEGORIES = sizeof(CATEGORIES) / sizeof(CATEGORIES[0]);

// The "N / M" strip both lists sit above is the theme's now
// (BaseTheme::pageIndicatorRect): drawList reserves it out of the rect it is
// handed and paints the count, so neither render() nor the page maths in
// loop() reserves anything of its own for it.

// The device only has an EPUB reader (the file browser allow-list is
// EPUB/XTC/TXT/MD/BMP — see FileBrowserActivity.cpp). Other BookFusion
// formats (PDF, audio, etc.) appear in the API responses but can't be opened
// here, so we render them with a strike-through and refuse the download.
bool bookFusionFormatIsEpub(const BookFusionBook& book) {
  if (book.format[0] == '\0') return true;  // Default in the parser is "epub".
  return strcasecmp(book.format, "epub") == 0;
}

// Image-heavy EPUBs strain the C3's ~380 KB RAM and slow / crash the renderer.
// Above this size we ask the user to confirm before downloading. File size is a
// good-enough proxy for "lots of pictures" — text-only EPUBs rarely approach it.
constexpr uint32_t LARGE_BOOK_WARN_BYTES = 10u * 1024 * 1024;  // 10 MB

bool bookFusionBookIsLarge(const BookFusionBook& book) { return book.downloadSize >= LARGE_BOOK_WARN_BYTES; }

std::string bookFusionExpectedFilename(const BookFusionBook& book) {
  std::string baseName = book.title;
  if (book.authors[0] != '\0') {
    baseName += " - ";
    baseName += book.authors;
  }
  char ext[8] = "epub";
  if (book.format[0] != '\0') {
    size_t i = 0;
    for (; i < sizeof(ext) - 1 && book.format[i] != '\0'; i++) {
      ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(book.format[i])));
    }
    ext[i] = '\0';
  }
  return "/" + StringUtils::sanitizeFilename(baseName) + "." + ext;
}

}  // namespace

void BookFusionBrowserActivity::onEnter() {
  Activity::onEnter();

  if (!BF_TOKEN_STORE.hasToken()) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_BF_NO_TOKEN_MSG), sizeof(errorMsg));
    requestUpdate();
    return;
  }

  // Connect WiFi and fetch the user's bookshelves up front so the category
  // menu shows the full [categories + shelves] list on the first render —
  // the user doesn't have to enter a category and back out to make shelves
  // appear. The wait is absorbed into the initial "Loading..." screen
  // they'd see anyway when picking any category.
  pendingWifiAction = WIFI_FOR_MENU;
  if (WiFi.status() == WL_CONNECTED) {
    loadShelvesAndShowMenu();
    return;
  }

  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::handleCategorySelection() {
  // Route selection to either a functional category or a user shelf.
  // The unified-menu layout (see render()) is [categories, shelves] — the
  // folder icon visually delimits the shelves, so no separator row is needed.
  if (selectedCategory < NUM_CATEGORIES) {
    currentCategory = selectedCategory;
    currentBookshelfId = 0;
    currentBookshelfName[0] = '\0';
  } else if (bookshelvesLoaded) {
    const int shelfIdx = selectedCategory - NUM_CATEGORIES;
    currentBookshelfId = bookshelves.shelves[shelfIdx].id;
    strlcpy(currentBookshelfName, bookshelves.shelves[shelfIdx].name, sizeof(currentBookshelfName));
    currentCategory = -1;  // unused while a shelf filter is active
  } else {
    return;
  }
  currentPage = 1;

  if (WiFi.status() == WL_CONNECTED) {
    loadPage(1);
    return;
  }

  // WiFi dropped between activity entry and category pick — re-prompt and
  // resume into the page fetch, not the menu, after reconnecting.
  pendingWifiAction = WIFI_FOR_PAGE;
  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookFusionBrowserActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
}

void BookFusionBrowserActivity::onWifiSelectionComplete(bool success) {
  if (!success) {
    state = ERROR;
    strlcpy(errorMsg, tr(STR_WIFI_CONN_FAILED), sizeof(errorMsg));
    requestUpdate();
    return;
  }
  if (pendingWifiAction == WIFI_FOR_MENU) {
    loadShelvesAndShowMenu();
    return;
  }
  currentPage = 1;
  loadPage(1);
}

void BookFusionBrowserActivity::loadShelvesAndShowMenu() {
  {
    RenderLock lock(*this);
    state = LOADING;
  }
  requestUpdate(true);

  // Errors are silent — the menu still shows the 5 functional categories
  // even if the shelf list fails to load (e.g. server outage). The flag flips
  // either way so we don't retry-storm.
  if (!bookshelvesLoaded) {
    {
      // Borrow the framebuffer for the TLS session. "Loading…" is already on
      // screen and nothing renders during the call, so holding the render lock
      // here is safe. See TlsFramebufferBorrow.
      TlsFramebufferBorrow borrow(renderer);
      BookFusionSyncClient::searchBookshelves(bookshelves);
    }
    bookshelvesLoaded = true;
  }

  {
    RenderLock lock(*this);
    state = CATEGORY_SELECTION;
  }
  requestUpdate();
}

void BookFusionBrowserActivity::loadPage(int page) {
  {
    RenderLock lock(*this);
    state = LOADING;
    selectedIndex = 0;
  }
  requestUpdate(true);

  // Pick the right list/sort depending on whether we're filtering by shelf or category.
  // Shelf mode bypasses the categorised lists ("currently_reading" etc.) and instead
  // sends `bookshelf_id` so the server returns books from that shelf only.
  const char* listParam = (currentBookshelfId != 0) ? nullptr : CATEGORIES[currentCategory].list;
  const char* sortParam = (currentBookshelfId != 0) ? nullptr : CATEGORIES[currentCategory].sort;
  auto err = BookFusionSyncClient::NETWORK_ERROR;
  {
    // Borrow the framebuffer for the TLS session. "Loading…" is already
    // on screen; nothing renders during the call.
    TlsFramebufferBorrow borrow(renderer);
    err = BookFusionSyncClient::searchBooks(page, searchResult, listParam, sortParam, currentBookshelfId);
  }

  if (err != BookFusionSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, BookFusionSyncClient::errorString(err), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  if (searchResult.count == 0) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_BF_NO_BOOKS), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  for (int i = 0; i < searchResult.count; ++i) {
    downloadedFlags[i] = Storage.exists(bookFusionExpectedFilename(searchResult.books[i]).c_str());
  }

  {
    RenderLock lock(*this);
    state = BROWSING;
    currentPage = page;
  }
  requestUpdate();
}

void BookFusionBrowserActivity::startDownload(int bookIndex) {
  const auto& book = searchResult.books[bookIndex];

  // BookFusion shelves can contain PDF and audio books alongside EPUBs. The
  // browse list renders the non-EPUB rows with a strike-through; pressing
  // Confirm on one of them lands here. Bail with a clear message rather than
  // burning bandwidth on a file the device can't open.
  if (!bookFusionFormatIsEpub(book)) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_BF_FORMAT_UNSUPPORTED), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = DOWNLOADING;
    downloadScreenPainted = false;      // Force a full repaint for this book's layout
    downloadTotal = book.downloadSize;  // Drives the "Filesize: … (approx. …)" line
    strlcpy(downloadTitle, book.title, sizeof(downloadTitle));
    strlcpy(downloadAuthor, book.authors[0] != '\0' ? book.authors : tr(STR_UNKNOWN_AUTHOR), sizeof(downloadAuthor));
    downloadedCoverPath[0] = '\0';  // Cleared until pre-gen succeeds below
    strlcpy(downloadStatus, tr(STR_CONNECTING), sizeof(downloadStatus));
  }
  // Non-blocking: the render task draws the "Connecting…" frame in parallel
  // with the cover fetch below. Blocking here (requestUpdateAndWait) would
  // serialise a full ~1 s e-ink refresh in front of every other step.
  requestUpdate(true);

  // Build destination path: "/Title - Author.ext" (sanitized). All inputs come
  // from the search result — no network call needed here.
  std::string baseName = book.title;
  if (book.authors[0] != '\0') {
    baseName += " - ";
    baseName += book.authors;
  }

  char ext[8] = "epub";
  if (book.format[0] != '\0') {
    size_t i = 0;
    for (; i < sizeof(ext) - 1 && book.format[i] != '\0'; i++) {
      ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(book.format[i])));
    }
    ext[i] = '\0';
  }

  const std::string filename = "/" + StringUtils::sanitizeFilename(baseName) + "." + ext;
  LOG_DBG("BFB", "Downloading book_id=%lu -> %s", static_cast<unsigned long>(book.id), filename.c_str());

  // Pre-fetch the cover before the pre-signed-URL round trip. `book.coverUrl`
  // is already in memory from the search response. The cover is tiny (~30 KB)
  // relative to the EPUB; landing it on disk first lets the Downloading card
  // show it above the title. If the API didn't include a cover URL we fall
  // back to extracting one from the EPUB after the download finishes (below).
  Epub epub(filename, "/.crosspoint");
  epub.clearCache();
  epub.setupCacheDir();
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  bool apiCoverOk = false;
  // The API cover is the ONLY cover this flow trusts (the EPUB-derived
  // fallback is deliberately disabled below — BookFusion EPUBs carry broken
  // covers), so try hard, in two phases that each lend the idle framebuffer to
  // the subsystem that needs it: TlsFramebufferBorrow for the CDN fetch (an
  // un-borrowed fetch OOMs silently — the original no-cover bug), then a
  // JpegScratchLease for the JPEG→BMP conversions (the ~20KB decoder + MCU
  // buffer OOM on this heap otherwise — observed on-device). The two leases
  // never alias: the fetch's TLS client is destroyed before its borrow ends,
  // and the convert phase re-registers the buffer fresh. Transient CDN
  // failures get retried; the queued "Connecting…" frame just paints after a
  // lease releases (downloadScreenPainted stays false → full paint).
  constexpr int kMaxCoverAttempts = 3;
  for (int attempt = 1; attempt <= kMaxCoverAttempts && !apiCoverOk; attempt++) {
    bool fetched = false;
    {
      TlsFramebufferBorrow borrow(renderer);
      fetched = BookFusionCoverCache::download(book.coverUrl, epub);
    }
    if (!fetched) {
      LOG_ERR("BFB", "API cover fetch attempt %d/%d failed for book_id=%lu", attempt, kMaxCoverAttempts,
              static_cast<unsigned long>(book.id));
      continue;
    }
    {
      RenderLock lock(*this);
      JpegScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
      apiCoverOk = BookFusionCoverCache::convert(epub, coverHeight, downloadedCoverPath, sizeof(downloadedCoverPath));
      downloadScreenPainted = false;  // lease scrambled the framebuffer
    }
    if (!apiCoverOk) {
      LOG_ERR("BFB", "API cover convert attempt %d/%d failed for book_id=%lu", attempt, kMaxCoverAttempts,
              static_cast<unsigned long>(book.id));
    }
  }
  if (apiCoverOk) {
    LOG_DBG("BFB", "Pre-fetched BookFusion API cover before EPUB download");
  }

  // Retry the fetch a couple of times: BookFusion's pre-signed URLs sometimes
  // drop mid-transfer and hand back a truncated EPUB (caught by the
  // expectedSize cross-check in downloadToFile). The pre-signed URL is fetched
  // fresh each attempt because it can expire between tries.
  constexpr int kMaxDownloadAttempts = 3;
  HttpDownloader::DownloadError dlResult = HttpDownloader::HTTP_ERROR;
  for (int attempt = 1; attempt <= kMaxDownloadAttempts; attempt++) {
    // Fetch the pre-signed download URL from BookFusion. Borrow the framebuffer
    // for this TLS session (the guard's render lock briefly defers the parallel
    // cover frame, which is harmless); its dtor closes the API connection — the
    // download itself goes to a different host (CDN).
    auto urlErr = BookFusionSyncClient::NETWORK_ERROR;
    {
      TlsFramebufferBorrow borrow(renderer);
      urlErr = BookFusionSyncClient::getDownloadUrl(book.id, downloadUrl, sizeof(downloadUrl));
    }
    if (urlErr != BookFusionSyncClient::OK) {
      {
        RenderLock lock(*this);
        state = ERROR;
        if (urlErr == BookFusionSyncClient::NOT_FOUND) {
          strlcpy(errorMsg, tr(STR_BF_BOOK_UNAVAILABLE), sizeof(errorMsg));
        } else {
          strlcpy(errorMsg, BookFusionSyncClient::errorString(urlErr), sizeof(errorMsg));
        }
      }
      requestUpdate();
      return;
    }

    {
      RenderLock lock(*this);
      // The getDownloadUrl borrow above left TLS garbage in the framebuffer:
      // the next paint must be a full one, not the dynamic-band shortcut.
      downloadScreenPainted = false;
      strlcpy(downloadStatus, tr(STR_DOWNLOAD_WAIT), sizeof(downloadStatus));
    }
    // Paint the static info card (title / author / filesize+estimate) NOW and
    // wait for it: the e-ink holds it with no RAM, and the screen stays frozen
    // on it for the whole transfer. No progress bar — the framebuffer is lent
    // to TLS below, so nothing can render until the download finishes. That
    // trade buys the CDN session the framebuffer arena for its handshake and
    // ~17KB record buffers instead of squeezing them into the browser's ~25KB
    // largest block, which is what dropped transfers mid-book.
    requestUpdateAndWait();

    {
      TlsFramebufferBorrow borrow(renderer);
      // Serial-only heartbeat so a long transfer is observable while the
      // screen is frozen. (The downloader's own client is destroyed — and its
      // arena memory freed — inside downloadToFile, before the borrow ends.)
      size_t lastLoggedMB = 0;
      dlResult = HttpDownloader::downloadToFile(
          downloadUrl, filename,
          [&lastLoggedMB](const size_t downloaded, const size_t total) {
            const size_t mb = downloaded >> 20;
            if (mb > lastLoggedMB) {
              lastLoggedMB = mb;
              if (total > 0) {
                LOG_DBG("BFB", "Download progress: %u/%u MB", (unsigned)mb, (unsigned)(total >> 20));
              } else {
                LOG_DBG("BFB", "Download progress: %u MB", (unsigned)mb);
              }
            }
          },
          true, static_cast<size_t>(book.downloadSize));
    }

    {
      // The borrow scrambled the framebuffer again — every downstream paint
      // (retrying / Saving… / error / complete popup) must be a full repaint.
      RenderLock lock(*this);
      downloadScreenPainted = false;
    }

    if (dlResult == HttpDownloader::OK) {
      break;
    }

    LOG_ERR("BFB", "Download attempt %d/%d failed for book_id=%lu", attempt, kMaxDownloadAttempts,
            static_cast<unsigned long>(book.id));

    // More attempts left — surface a brief "retrying" notice on the download
    // screen and loop. The truncated/partial file is already removed by
    // downloadToFile on failure, so the next attempt starts clean.
    if (attempt < kMaxDownloadAttempts) {
      RenderLock lock(*this);
      strlcpy(downloadStatus, tr(STR_DOWNLOAD_RETRYING), sizeof(downloadStatus));
      requestUpdate(true);
    }
  }

  if (dlResult != HttpDownloader::OK) {
    {
      RenderLock lock(*this);
      state = ERROR;
      strlcpy(errorMsg, tr(STR_DOWNLOAD_FAILED), sizeof(errorMsg));
    }
    requestUpdate();
    return;
  }

  // EPUB transfer done — the remaining work (metadata parse, sidecar write,
  // optional cover fallback, progress sync) is silent on the network and SD
  // bus but can take 1-2 s on a big book. Flip the status so the user knows
  // we're still doing something useful.
  strlcpy(downloadStatus, tr(STR_SAVING), sizeof(downloadStatus));
  requestUpdate(true);

  // Save sidecar so BookFusionSyncActivity can find the book_id for this file.
  BookFusionBookIdStore::saveBookId(filename.c_str(), book.id);

  // Build metadata. clearCache already ran above so any pre-existing cache for
  // this filename is gone; epub.load(true) rebuilds it from the freshly-downloaded
  // EPUB. The pre-fetched cover lives at /thumb_<H>.bmp inside the cache dir,
  // which load() does not touch, so it survives.
  LOG_DBG("BFB", "Loading EPUB metadata for recent-books entry");
  bool loadSuccess = false;
  {
    // The metadata build streams the EPUB zip through a 32KB inflate
    // dictionary — the single contiguous allocation this heap cannot serve
    // once fragmented (observed on-device: largest block 32756, 12 bytes
    // short). Lease the framebuffer as the dictionary, same as section builds
    // do (Section.cpp). The "Saving…" frame is already on the panel and
    // nothing renders while the lock is held.
    RenderLock lock(*this);
    InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
    loadSuccess = epub.load(true, true);  // buildIfMissing=true, skipLoadingCss=true (we only need metadata)
    downloadScreenPainted = false;        // lease scrambled the framebuffer
  }
  LOG_DBG("BFB", "EPUB load result: %s", loadSuccess ? "SUCCESS" : "FAILED");

  if (loadSuccess) {
    RECENT_BOOKS.addBook(filename, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath());

    // Persist BookFusion's organisational metadata (categories / bookshelves /
    // lists) — these aren't embedded in the EPUB, so the Book Details view has
    // no other source for them. Written after load() (which rebuilt the cache
    // dir) so it survives; an empty meta just clears any stale sidecar.
    BookFusionMeta bfMeta;
    bfMeta.categories = book.categories;
    bfMeta.bookshelves = book.bookshelves;
    bfMeta.lists = book.lists;
    BookFusionMetaStore::save(epub.getCachePath(), bfMeta);

    // No EPUB-cover fallback: BookFusion-served EPUBs frequently carry broken or
    // unreliable cover images, so the only cover we trust is the already-normalised
    // image from the API (cached above by BookFusionCoverCache::refresh). If the
    // pre-download attempts all failed, this is the last chance: WiFi is still up,
    // the heavy transfer is done, and the heap is at its calmest — retry the API
    // cover now rather than ship the book without one. Only if THIS also fails does
    // the cover stay unset (DOWNLOAD_COMPLETE popup falls back to text-only and the
    // library shows a placeholder — never a bad EPUB-derived cover).
    if (!apiCoverOk) {
      bool fetched = false;
      {
        TlsFramebufferBorrow borrow(renderer);
        fetched = BookFusionCoverCache::download(book.coverUrl, epub);
        downloadScreenPainted = false;  // borrow scrambled the framebuffer (render task is blocked here)
      }
      if (fetched) {
        RenderLock lock(*this);
        JpegScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
        apiCoverOk = BookFusionCoverCache::convert(epub, coverHeight, downloadedCoverPath, sizeof(downloadedCoverPath));
        downloadScreenPainted = false;  // lease scrambled the framebuffer
      }
    }
    if (!apiCoverOk) {
      LOG_ERR("BFB", "BookFusion API cover unavailable after all attempts; no EPUB fallback (text-only)");
    }

    // Pull the user's BookFusion reading position so the book opens where they
    // left off on another device. Best-effort — failure here doesn't fail the
    // download. WiFi is still up, the user is already in the "Downloading..."
    // wait so the extra round trip is invisible. Borrowed like every other TLS
    // session in this activity; the borrow's dtor closes the API connection.
    BookFusionPosition remotePos{};
    auto syncErr = BookFusionSyncClient::NETWORK_ERROR;
    {
      TlsFramebufferBorrow borrow(renderer);
      syncErr = BookFusionSyncClient::getProgress(book.id, remotePos);
      downloadScreenPainted = false;  // borrow scrambled the framebuffer
    }
    if (syncErr == BookFusionSyncClient::OK && remotePos.percentage > 0.0f) {
      const int spineCount = epub.getSpineItemsCount();
      if (remotePos.chapterIndex >= 0 && remotePos.chapterIndex < spineCount) {
        // progress.bin format (EpubReaderActivity.cpp:111-128):
        //   [0..1] spineIndex (LE uint16)
        //   [2..3] pageNumber (LE uint16)  — 0 = start of chapter; the per-chapter
        //                                    page count isn't known until the
        //                                    reader renders the spine item.
        FsFile progressFile;
        if (Storage.openFileForWrite("BFB", epub.getCachePath() + "/progress.bin", progressFile)) {
          const uint16_t spineIndex = static_cast<uint16_t>(remotePos.chapterIndex);
          uint8_t data[4] = {static_cast<uint8_t>(spineIndex & 0xFF), static_cast<uint8_t>((spineIndex >> 8) & 0xFF), 0,
                             0};
          progressFile.write(data, sizeof(data));
          progressFile.flush();
          progressFile.close();
          if (remotePos.updatedAt[0] != '\0') {
            BookFusionBookIdStore::saveLastSyncAt(filename.c_str(), remotePos.updatedAt);
          }
          // The library row reads its displayed % straight from RECENT_BOOKS, which
          // addBook() seeded as "unknown" (-1). Nothing else updates it until the
          // reader renders a page and calls saveProgress(), so without this the synced
          // progress is invisible in the library until the book is opened. BookFusion
          // is authoritative for reading position, so display its book-level percentage
          // directly rather than recomputing from chapterIndex (which is unreliable —
          // it defaults to 0 whenever the API omits chapter_index, hiding real progress).
          const auto progressPercent = static_cast<int8_t>(std::clamp(remotePos.percentage, 0.0f, 100.0f) + 0.5f);
          RECENT_BOOKS.updateProgress(filename, progressPercent);
          LOG_DBG("BFB", "Synced BookFusion position: chapter %d, %d%% (BookFusion authoritative)",
                  remotePos.chapterIndex, progressPercent);
        }
      }
    } else if (syncErr != BookFusionSyncClient::OK && syncErr != BookFusionSyncClient::NOT_FOUND) {
      LOG_DBG("BFB", "BookFusion position sync skipped: %s", BookFusionSyncClient::errorString(syncErr));
    }
  }

  LOG_DBG("BFB", "Download complete, cache cleared and cover regenerated for book_id=%lu",
          static_cast<unsigned long>(book.id));

  // Force the library to re-scan on its next open. The new file lands in the SD
  // root, whose FAT mtime doesn't change on a child add, so the library index's
  // mtime-based validation can't detect it on its own.
  LibraryActivity::invalidateIndexCache();

  downloadedFlags[bookIndex] = true;

  {
    RenderLock lock(*this);
    state = DOWNLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void BookFusionBrowserActivity::loop() {
  if (state == WIFI_SELECTION || state == LOADING || state == DOWNLOADING) {
    return;
  }

  if (state == CATEGORY_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const bool showShelves = bookshelvesLoaded && bookshelves.count > 0;
    const int total = NUM_CATEGORIES + (showShelves ? bookshelves.count : 0);
    // Visual page size — same calculation drawList uses internally — so a
    // long-press jump matches one screen of items exactly.
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      handleCategorySelection();
      return;
    }
    buttonNavigator.onNextRelease([this, total] {
      selectedCategory = ButtonNavigator::nextIndex(selectedCategory, total);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, total] {
      selectedCategory = ButtonNavigator::previousIndex(selectedCategory, total);
      requestUpdate();
    });
    // Full Touch: a vertical swipe turns a page, matching the held side key.
    if (TouchListNav::pageSwipe(mappedInput, total, pageItems, selectedCategory)) {
      requestUpdate();
      return;
    }
    buttonNavigator.onNextContinuous([this, total, pageItems] {
      selectedCategory = ButtonNavigator::nextPageIndex(selectedCategory, total, pageItems);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this, total, pageItems] {
      selectedCategory = ButtonNavigator::previousPageIndex(selectedCategory, total, pageItems);
      requestUpdate();
    });
    return;
  }

  if (state == ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (BF_TOKEN_STORE.hasToken()) {
        {
          RenderLock lock(*this);
          state = CATEGORY_SELECTION;
        }
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == CONFIRM_LARGE_DOWNLOAD) {
    // Confirm proceeds with the download; Back cancels and returns to the list.
    // wasPressed (not wasReleased) so the press that opened this screen — already
    // consumed in BROWSING — can't immediately confirm on its own release.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const int idx = pendingDownloadIndex;
      pendingDownloadIndex = -1;
      if (idx >= 0 && idx < searchResult.count) {
        startDownload(idx);
      } else {
        RenderLock lock(*this);
        state = BROWSING;
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      pendingDownloadIndex = -1;
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == BROWSING) {
    const int totalItems = searchResult.count;

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state = CATEGORY_SELECTION;
      }
      requestUpdate();
      return;
    }

    int tappedIndex;
    switch (TouchListNav::tapRow(mappedInput, listRect(), searchResult.count, selectedIndex,
                                 /*hasSubtitle=*/true, tappedIndex)) {
      case TouchListNav::TapResult::SelectionMoved:
        selectedIndex = tappedIndex;
        requestUpdate();
        return;
      case TouchListNav::TapResult::Activated:
        activateSelectedBook();
        return;
      case TouchListNav::TapResult::None:
        break;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelectedBook();
      return;
    }

    // Tap = step through the current page. Falling off either end loads the
    // adjacent API page so paging works without requiring the hold-Up/Down
    // gesture. Coming back from a forward fall, the cursor lands on the
    // first item of the new page; coming back from a backward fall, it
    // lands on the last item — both keep "one step at a time" feeling
    // continuous across the page break.
    buttonNavigator.onNextRelease([this, totalItems] {
      if (selectedIndex == totalItems - 1) {
        if (searchResult.hasMore) {
          loadPage(currentPage + 1);
          return;
        }
        // On the last item of the last page — wrap forward to page 1 if we
        // know there's more than one page. Mirrors the tap-Up wrap from page 1
        // back to the last page. Lands on item 0 of page 1 so the continuous
        // step keeps going forward.
        if (currentPage > 1) {
          loadPage(1);
          return;
        }
      }
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this, totalItems] {
      // First item, not on page 1 → step back a page.
      // First item, already on page 1, and we know there are more pages →
      //   wrap all the way around to the last page.
      // Either case lands on the last item of the loaded page so backward
      // stepping feels continuous across the page break.
      if (selectedIndex == 0) {
        constexpr int perPage = BookFusionSearchResult::MAX_BOOKS;
        const int lastPage = searchResult.totalCount > 0 ? (searchResult.totalCount + perPage - 1) / perPage : 0;
        int target = 0;
        if (currentPage > 1) {
          target = currentPage - 1;
        } else if (lastPage > 1) {
          target = lastPage;
        }
        if (target > 0) {
          loadPage(target);
          if (state == BROWSING && searchResult.count > 0) {
            selectedIndex = searchResult.count - 1;
            requestUpdate();
          }
          return;
        }
      }
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    // Hold = flip to the next/previous API page. Pagination is server-side
    // (only `hasMore` is known, not the total page count), so we just bounce off
    // the ends rather than wrapping.
    const auto nextApiPage = [this] {
      if (searchResult.hasMore) {
        loadPage(currentPage + 1);
        return;
      }
      // No more pages forward — wrap to page 1 (jump semantics: land on
      // item 0). Mirrors the hold-Up wrap to the last page from page 1.
      if (currentPage > 1) {
        loadPage(1);
      }
    };

    const auto previousApiPage = [this] {
      if (currentPage > 1) {
        loadPage(currentPage - 1);
        return;
      }
      // Already on page 1 — wrap to the last page when we know how many there
      // are (Total-Count). Lands on item 0 of the last page (jump semantics)
      // rather than the last item (which is the tap-Up continuous-step
      // behaviour). If totalCount is unknown we leave hold-Up as a no-op.
      constexpr int perPage = BookFusionSearchResult::MAX_BOOKS;
      const int lastPage = searchResult.totalCount > 0 ? (searchResult.totalCount + perPage - 1) / perPage : 0;
      if (lastPage > 1) {
        loadPage(lastPage);
      }
    };

    // Full Touch: a vertical swipe flips API pages, matching the held side
    // key. Return immediately after dispatch — loadPage can leave BROWSING, so
    // the button bindings below must not run against a state we just left. The
    // lambdas' own bounds checks provide the one-page inertness pageSwipeDelta
    // deliberately doesn't (see its doc comment).
    switch (TouchListNav::pageSwipeDelta(mappedInput)) {
      case +1:
        nextApiPage();
        return;
      case -1:
        previousApiPage();
        return;
      default:
        break;
    }

    buttonNavigator.onNextContinuous(nextApiPage);
    buttonNavigator.onPreviousContinuous(previousApiPage);
  }
}

void BookFusionBrowserActivity::activateSelectedBook() {
  if (selectedIndex < searchResult.count) {
    // Gate large image-heavy EPUBs behind a confirm screen — the API gives us
    // download_size up front, so we can ask before spending the transfer. PDFs
    // and other formats are rejected inside startDownload regardless.
    const auto& book = searchResult.books[selectedIndex];
    if (bookFusionFormatIsEpub(book) && bookFusionBookIsLarge(book)) {
      pendingDownloadIndex = selectedIndex;
      {
        RenderLock lock(*this);
        state = CONFIRM_LARGE_DOWNLOAD;
      }
      requestUpdate();
    } else {
      startDownload(selectedIndex);
    }
  }
}

void BookFusionBrowserActivity::drawDownloadDynamic(const int statusY) {
  const int pageWidth = renderer.getScreenWidth();

  // Clear just the status band to white before (re)drawing it. On the full
  // path the whole screen was already cleared so this is a harmless no-op; on
  // the partial path it wipes the previous phase label (Connecting / retrying
  // / Saving differ in width) so it doesn't ghost in the retained framebuffer.
  renderer.fillRect(0, statusY, pageWidth, 40, false);

  // Phase label set by startDownload (Connecting / wait notice / Saving /
  // retrying). Falls back to the generic STR_DOWNLOADING if somehow blank.
  const char* status = (downloadStatus[0] != '\0') ? downloadStatus : tr(STR_DOWNLOADING);
  renderer.drawCenteredText(UI_10_FONT_ID, statusY, status, true, EpdFontFamily::BOLD);
}

// List body of the BROWSING state, between the header and the page-indicator
// strip. Shared by render() and the loop()'s tap hit-testing so the two can
// never disagree.
Rect BookFusionBrowserActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // No explicit indicator reserve: drawList takes the counter strip out of the
  // rect it is handed (BaseTheme::pageIndicatorRect).
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void BookFusionBrowserActivity::render(RenderLock&&) {
  // Fast path: a progress tick on an already-painted Downloading screen. In
  // single-buffer mode the framebuffer survives between renders, so the header,
  // cover and title are still present — repaint only the dynamic status/progress
  // band instead of wiping everything and re-parsing the cover BMP from SD (the
  // SD read contends with the download's own writes on the HalStorage mutex).
  if (state == DOWNLOADING && downloadScreenPainted) {
    drawDownloadDynamic(downloadStatusY);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const char* headerTitle;
  if (state == CATEGORY_SELECTION) {
    headerTitle = tr(STR_BF_BROWSE_LIBRARY);
  } else if (currentBookshelfId != 0) {
    // Browsing inside a user shelf — header reads the shelf's own name.
    headerTitle = currentBookshelfName;
  } else {
    headerTitle = I18N.get(CATEGORIES[currentCategory].nameId);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle);

  if (state == CATEGORY_SELECTION) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight;

    // Unified menu layout:
    //   [0..NUM_CATEGORIES-1] → functional categories (book/star/arrow/check/library icons)
    //   [NUM_CATEGORIES..end] → user shelves (folder icon)
    // No textual "── Bookshelves ──" separator: the icon change between
    // categories and shelves is itself the visual delimiter.
    const bool showShelves = bookshelvesLoaded && bookshelves.count > 0;
    const int total = NUM_CATEGORIES + (showShelves ? bookshelves.count : 0);

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, total, selectedCategory,
        [this](int index) -> std::string {
          if (index < NUM_CATEGORIES) {
            return std::string(I18N.get(CATEGORIES[index].nameId));
          }
          return std::string(bookshelves.shelves[index - NUM_CATEGORIES].name);
        },
        nullptr,
        [](int index) -> UIIcon {
          if (index < NUM_CATEGORIES) return CATEGORIES[index].icon;
          return UIIcon::Folder;
        },
        nullptr, true);

    // The page count is drawn by drawList itself, in the strip every screen
    // reserves for it — the category list pages by its own rows, so nothing
    // here needs to say it differently.

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == WIFI_SELECTION || state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, errorMsg, true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CONFIRM_LARGE_DOWNLOAD) {
    const int maxWidth = pageWidth - 40;
    const bool valid = pendingDownloadIndex >= 0 && pendingDownloadIndex < searchResult.count;
    const BookFusionBook& book = searchResult.books[valid ? pendingDownloadIndex : 0];

    auto title = renderer.truncatedText(UI_10_FONT_ID, valid ? book.title : "", maxWidth);
    const auto warnLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_BF_LARGE_BOOK_WARNING), maxWidth, 4);
    const int smallH = renderer.getTextHeight(SMALL_FONT_ID) + 6;

    // Centre the title + size + warning stack around the screen midpoint.
    const int blockH = 30 + 34 + static_cast<int>(warnLines.size()) * smallH;
    int y = pageHeight / 2 - blockH / 2;

    renderer.drawCenteredText(UI_10_FONT_ID, y, title.c_str(), true, EpdFontFamily::BOLD);
    y += 30;
    char sizeText[24];
    snprintf(sizeText, sizeof(sizeText), "%.1f MB", (valid ? book.downloadSize : 0) / (1024.0f * 1024.0f));
    renderer.drawCenteredText(UI_10_FONT_ID, y, sizeText, true, EpdFontFamily::BOLD);
    y += 34;
    for (const auto& line : warnLines) {
      renderer.drawCenteredText(SMALL_FONT_ID, y, line.c_str());
      y += smallH;
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DOWNLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == DOWNLOADING) {
    // Frozen info card: cover (when pre-fetched) above title / author /
    // filesize+estimate / status. No progress bar — this frame is painted once
    // and then the screen holds it while the framebuffer is lent to TLS for
    // the whole transfer (see startDownload). Only the status line repaints
    // (retrying / Saving), via the fast path at the top of render().
    constexpr int lineH = 34;
    constexpr int coverTextGap = 20;
    const int maxWidth = pageWidth - 40;
    char line[128];

    // Centre the cover + 4-line stack around the screen midpoint. Without a
    // cover this degrades to the plain centred text card.
    int y = pageHeight / 2 - 2 * lineH;
    if (downloadedCoverPath[0] != '\0') {
      FsFile coverFile;
      if (Storage.openFileForRead("BFB", downloadedCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
          const int coverH = metrics.homeCoverHeight;
          const int coverW = static_cast<int>(static_cast<float>(coverH) * static_cast<float>(bitmap.getWidth()) /
                                              static_cast<float>(bitmap.getHeight()));
          const int stackH = coverH + coverTextGap + 4 * lineH;
          const int coverY = pageHeight / 2 - stackH / 2;
          renderer.drawBitmap(bitmap, (pageWidth - coverW) / 2, coverY, coverW, coverH, 0.0f);
          y = coverY + coverH + coverTextGap;
        }
        coverFile.close();
      }
    }

    snprintf(line, sizeof(line), tr(STR_DOWNLOAD_TITLE_FORMAT), downloadTitle);
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, line, maxWidth).c_str(), true,
                              EpdFontFamily::BOLD);
    y += lineH;

    snprintf(line, sizeof(line), tr(STR_DOWNLOAD_AUTHOR_FORMAT), downloadAuthor);
    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, line, maxWidth).c_str());
    y += lineH;

    if (downloadTotal > 0) {
      // Duration estimate from the observed transfer rate (TLS decrypt + SD
      // write on the C3; measured 2.7MB in ~90s on hardware); "approx."
      // absorbs the spread.
      constexpr size_t kEstimatedBytesPerSec = 30 * 1024;
      const unsigned estSec =
          static_cast<unsigned>((downloadTotal + kEstimatedBytesPerSec - 1) / kEstimatedBytesPerSec);
      char sizeStr[16];
      snprintf(sizeStr, sizeof(sizeStr), "%.1f MB", downloadTotal / (1024.0f * 1024.0f));
      if (estSec > 90) {
        snprintf(line, sizeof(line), tr(STR_DOWNLOAD_SIZE_MIN_FORMAT), sizeStr, (estSec + 59) / 60);
      } else {
        snprintf(line, sizeof(line), tr(STR_DOWNLOAD_SIZE_SEC_FORMAT), sizeStr, estSec);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    }
    y += lineH;

    downloadStatusY = y;           // Shared with the fast-path partial repaint
    downloadScreenPainted = true;  // Card now lives in the persistent framebuffer
    drawDownloadDynamic(y);
    renderer.displayBuffer();
    return;
  }

  if (state == DOWNLOAD_COMPLETE) {
    // If pre-gen produced a cover, draw it above the text. Centre cover + text block
    // around pageHeight/2 by shifting text down by half the cover height + gap.
    int textBaseY = pageHeight / 2;
    if (downloadedCoverPath[0] != '\0') {
      FsFile coverFile;
      if (Storage.openFileForRead("BFB", downloadedCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
          const int coverH = metrics.homeCoverHeight;
          const int coverW = static_cast<int>(static_cast<float>(coverH) * static_cast<float>(bitmap.getWidth()) /
                                              static_cast<float>(bitmap.getHeight()));
          constexpr int coverTextGap = 20;
          const int coverY = pageHeight / 2 - (coverH + coverTextGap) / 2 - 15;
          renderer.drawBitmap(bitmap, (pageWidth - coverW) / 2, coverY, coverW, coverH, 0.0f);
          textBaseY = coverY + coverH + coverTextGap;
        }
        coverFile.close();
      }
    }

    renderer.drawCenteredText(UI_10_FONT_ID, textBaseY, tr(STR_BF_DOWNLOAD_COMPLETE), true, EpdFontFamily::BOLD);
    const int maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, textBaseY + 30, title.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // BROWSING state — subtitle list with title + author + BookFusion icon (matches
  // RecentBooksActivity).
  const Rect contentRect = listRect();

  // These pages are the SERVER's, not this rect's rows, so the shared counter
  // strip gets our wording. Prefer the exact count when BookFusion returned a
  // `Total-Count` response header (we read it into searchResult.totalCount in
  // the client); if the header is missing, fall back to the `+` suffix so the
  // user at least knows more pages exist.
  char indicator[24];
  if (searchResult.totalCount > 0) {
    const int perPage = BookFusionSearchResult::MAX_BOOKS;
    const int totalPages = (searchResult.totalCount + perPage - 1) / perPage;
    snprintf(indicator, sizeof(indicator), "%d / %d", currentPage, totalPages);
  } else if (searchResult.hasMore) {
    snprintf(indicator, sizeof(indicator), "%d / %d+", currentPage, currentPage);
  } else {
    snprintf(indicator, sizeof(indicator), "%d / %d", currentPage, currentPage);
  }

  GUI.drawList(
      renderer, contentRect, searchResult.count, selectedIndex,
      [this](int index) -> std::string { return std::string(searchResult.books[index].title); },
      [this](int index) -> std::string { return std::string(searchResult.books[index].authors); },
      [this](int index) { return downloadedFlags[index] ? UIIcon::Check : UIIcon::BookFusion; }, nullptr, false,
      nullptr, indicator);

  // Overlay a strike-through on rows whose book isn't an EPUB so the user can
  // see the book exists but at a glance knows it can't be opened here. The rows
  // come from the theme's own geometry (so the overlay cannot page differently
  // from the list it marks up); only the in-row line offsets (title at itemY+7,
  // subtitle at itemY+30) are replicated here, and they are the one thing to
  // update if a theme ever moves them.
  {
    const auto geo = GUI.listGeometry(contentRect, selectedIndex, /*hasSubtitle=*/true);
    const int titleStrikeY = 7 + renderer.getLineHeight(UI_10_FONT_ID) / 2;
    const int subtitleStrikeY = 30 + renderer.getLineHeight(SMALL_FONT_ID) / 2;
    const int strikeLeft = metrics.contentSidePadding + 8;
    const int strikeRight = pageWidth - metrics.contentSidePadding - 8;
    for (int i = geo.pageStart; i < searchResult.count && i < geo.pageStart + geo.pageItems; ++i) {
      if (bookFusionFormatIsEpub(searchResult.books[i])) continue;
      const int itemY = contentRect.y + (i % geo.pageItems) * geo.rowHeight;
      renderer.drawLine(strikeLeft, itemY + titleStrikeY, strikeRight, itemY + titleStrikeY, true);
      renderer.drawLine(strikeLeft, itemY + subtitleStrikeY, strikeRight, itemY + subtitleStrikeY, true);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
