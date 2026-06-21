#include "BookFusionBrowserActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
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
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

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

// Height of the "N / M" strip drawn just above the button hints. Used by
// both loop() (when sizing one visual page for long-press jumps) and render()
// (when reserving content area for the indicator) — must agree.
constexpr int categoryPageIndicatorH = 30;

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

bool bookFusionBookIsLarge(const BookFusionBook& book) {
  return book.downloadSize >= LARGE_BOOK_WARN_BYTES;
}

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

enum class CoverImageType { Unknown, Jpeg, Png };

std::string normalizeBookFusionCoverUrl(const char* coverUrl) {
  if (!coverUrl || coverUrl[0] == '\0') return {};

  std::string url = coverUrl;
  if (url.rfind("//", 0) == 0) {
    return "https:" + url;
  }
  if (url.rfind("/", 0) == 0) {
    return "https://www.bookfusion.com" + url;
  }
  return url;
}

CoverImageType detectCoverImageType(FsFile& file) {
  uint8_t header[8] = {};
  file.seek(0);
  const int read = file.read(header, sizeof(header));
  file.seek(0);

  if (read >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return CoverImageType::Jpeg;
  }
  if (read >= 8 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' && header[4] == 0x0D &&
      header[5] == 0x0A && header[6] == 0x1A && header[7] == 0x0A) {
    return CoverImageType::Png;
  }
  return CoverImageType::Unknown;
}

bool validateCoverBmp(const std::string& path) {
  FsFile file;
  if (!Storage.openFileForRead("BFB", path, file)) return false;

  Bitmap bitmap(file);
  const auto err = bitmap.parseHeaders();
  file.close();

  if (err != BmpReaderError::Ok) {
    LOG_ERR("BFB", "BookFusion cover BMP validation failed (err=%d): %s", static_cast<int>(err), path.c_str());
    return false;
  }
  return true;
}

bool convertBookFusionCoverImage(const std::string& srcPath, const std::string& destPath, bool thumbnail,
                                 int thumbTargetWidth, int thumbTargetHeight, bool crop) {
  FsFile src;
  if (!Storage.openFileForRead("BFB", srcPath, src)) return false;

  const CoverImageType type = detectCoverImageType(src);
  if (type == CoverImageType::Unknown) {
    LOG_ERR("BFB", "BookFusion cover has unsupported image signature: %s", srcPath.c_str());
    src.close();
    return false;
  }

  FsFile dest;
  if (!Storage.openFileForWrite("BFB", destPath, dest)) {
    src.close();
    return false;
  }

  bool success = false;
  if (thumbnail) {
    // Grayscale (not 1-bit) thumbnails. The 1-bit converter is less robust and
    // fails on some cover JPEGs, leaving thumb_<H>.bmp missing even though the
    // grayscale cover.bmp from the same source succeeds — which is why some
    // downloaded books showed a cover and others didn't. Epub::generateThumbBmp
    // uses grayscale for exactly this reason ("avoid reading issues", Epub.cpp);
    // matching it makes BookFusion thumbnails render reliably in the home cards
    // and the library grid.
    if (type == CoverImageType::Jpeg) {
      success = JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, dest, thumbTargetWidth, thumbTargetHeight);
    } else {
      success = PngToBmpConverter::pngFileToBmpStreamWithSize(src, dest, thumbTargetWidth, thumbTargetHeight);
    }
  } else {
    if (type == CoverImageType::Jpeg) {
      success = JpegToBmpConverter::jpegFileToBmpStream(src, dest, crop);
    } else {
      success = PngToBmpConverter::pngFileToBmpStream(src, dest, crop);
    }
  }

  src.close();
  dest.flush();
  dest.close();

  if (!success || !validateCoverBmp(destPath)) {
    LOG_ERR("BFB", "Failed to convert BookFusion cover BMP: %s", destPath.c_str());
    Storage.remove(destPath.c_str());
    return false;
  }

  return true;
}

bool cacheBookFusionApiCover(const BookFusionBook& book, const Epub& epub, int coverHeight, char* downloadedCoverPath,
                             size_t downloadedCoverPathLen) {
  const std::string coverUrl = normalizeBookFusionCoverUrl(book.coverUrl);
  if (coverUrl.empty()) {
    LOG_DBG("BFB", "BookFusion API did not include a cover URL for book_id=%lu", static_cast<unsigned long>(book.id));
    return false;
  }

  const std::string tempCoverPath = epub.getCachePath() + "/.bookfusion-cover";
  LOG_DBG("BFB", "Downloading BookFusion API cover for book_id=%lu", static_cast<unsigned long>(book.id));
  const auto downloadResult = HttpDownloader::downloadToFile(coverUrl, tempCoverPath, nullptr, false);
  if (downloadResult != HttpDownloader::OK) {
    LOG_ERR("BFB", "Failed to download BookFusion API cover for book_id=%lu", static_cast<unsigned long>(book.id));
    return false;
  }

  const int thumbTargetWidth = coverHeight * 0.6f;
  const int thumbTargetHeight = coverHeight;
  const std::string thumbPath = epub.getThumbBmpPath(coverHeight);
  const bool thumbOk =
      convertBookFusionCoverImage(tempCoverPath, thumbPath, true, thumbTargetWidth, thumbTargetHeight, true);

  // Prime both sleep-screen variants while WiFi is already on. SleepActivity
  // will later pick the one matching the user's cover mode.
  const bool fitOk = convertBookFusionCoverImage(tempCoverPath, epub.getCoverBmpPath(false), false, 0, 0, false);
  const bool cropOk = convertBookFusionCoverImage(tempCoverPath, epub.getCoverBmpPath(true), false, 0, 0, true);

  Storage.remove(tempCoverPath.c_str());

  if (thumbOk) {
    strlcpy(downloadedCoverPath, thumbPath.c_str(), downloadedCoverPathLen);
  }

  LOG_DBG("BFB", "BookFusion API cover cache result: thumb=%d fit=%d crop=%d", thumbOk, fitOk, cropOk);
  return thumbOk;
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
    BookFusionSyncClient::searchBookshelves(bookshelves);
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
  const auto err = BookFusionSyncClient::searchBooks(page, searchResult, listParam, sortParam, currentBookshelfId);

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
    downloadProgress = 0;
    downloadTotal = 0;
    lastProgressUpdateMs = 0;  // Reset progress update throttling
    strlcpy(downloadTitle, book.title, sizeof(downloadTitle));
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
  // is already in memory from the search response, so we can race the cover
  // download against the URL fetch instead of serialising them. The cover is
  // tiny (~30 KB) relative to the EPUB; landing it on disk first lets the
  // Downloading screen show the cover alongside the title and progress bar.
  // If the API didn't include a cover URL we fall back to extracting one from
  // the EPUB after the download finishes (see below).
  Epub epub(filename, "/.crosspoint");
  epub.clearCache();
  epub.setupCacheDir();
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  const bool apiCoverOk =
      cacheBookFusionApiCover(book, epub, coverHeight, downloadedCoverPath, sizeof(downloadedCoverPath));
  if (apiCoverOk) {
    LOG_DBG("BFB", "Pre-fetched BookFusion API cover before EPUB download");
    requestUpdate(true);  // Queue a second frame so the cover replaces the bare title.
  }

  // Fetch the pre-signed download URL from BookFusion (happens in parallel with
  // the cover frame the render task is now drawing).
  const auto urlErr = BookFusionSyncClient::getDownloadUrl(book.id, downloadUrl, sizeof(downloadUrl));
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

  const auto dlResult =
      HttpDownloader::downloadToFile(downloadUrl, filename, [this](const size_t downloaded, const size_t total) {
        // Throttle UI updates to every 2 seconds to avoid blocking download with slow e-ink refreshes
        const unsigned long currentMs = millis();
        const unsigned long timeSinceLastUpdate = currentMs - lastProgressUpdateMs;

        downloadProgress = downloaded;
        downloadTotal = total;

        // First byte arrived — flip the status from "Connecting…" to
        // "Downloading…". Done inside the callback (rather than up-front
        // before calling HttpDownloader) so the render task has time to paint
        // the "Connecting… + cover" frame first; otherwise the cover frame
        // gets coalesced away on the e-ink refresh queue. HttpDownloader now
        // always fires the callback, including for transfers without
        // Content-Length, so this branch is reliable.
        if (lastProgressUpdateMs == 0) {
          strlcpy(downloadStatus, tr(STR_DOWNLOADING), sizeof(downloadStatus));
        }

        // Update immediately for first progress report or every 2 seconds
        if (lastProgressUpdateMs == 0 || timeSinceLastUpdate >= 2000) {
          lastProgressUpdateMs = currentMs;
          requestUpdate(true);
        }
      });

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
  bool loadSuccess = epub.load(true, true);  // buildIfMissing=true, skipLoadingCss=true (we only need metadata)
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
    // image from the API (cached above by cacheBookFusionApiCover). If that failed,
    // leave the cover unset — the DOWNLOAD_COMPLETE popup falls back to text-only and
    // the library shows a placeholder, rather than surfacing a bad EPUB-derived cover.
    if (!apiCoverOk) {
      LOG_DBG("BFB", "BookFusion API cover unavailable; skipping EPUB fallback (text-only)");
    }

    // Pull the user's BookFusion reading position so the book opens where they
    // left off on another device. Best-effort — failure here doesn't fail the
    // download. WiFi is still up, the user is already in the "Downloading..."
    // wait so the extra round trip is invisible.
    BookFusionPosition remotePos{};
    const auto syncErr = BookFusionSyncClient::getProgress(book.id, remotePos);
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
          const auto progressPercent = static_cast<int8_t>(
              std::clamp(remotePos.percentage, 0.0f, 100.0f) + 0.5f);
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
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, categoryPageIndicatorH);

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

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
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
    buttonNavigator.onNextContinuous([this] {
      if (searchResult.hasMore) {
        loadPage(currentPage + 1);
        return;
      }
      // No more pages forward — wrap to page 1 (jump semantics: land on
      // item 0). Mirrors the hold-Up wrap to the last page from page 1.
      if (currentPage > 1) {
        loadPage(1);
      }
    });

    buttonNavigator.onPreviousContinuous([this] {
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
    });
  }
}

void BookFusionBrowserActivity::render(RenderLock&&) {
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
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - categoryPageIndicatorH;

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

    // Page indicator (same "N / M" format as BROWSING). The category list is
    // fully in memory so M is known exactly — no '+' suffix needed.
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, categoryPageIndicatorH);
    const int totalPages = (pageItems > 0) ? (total + pageItems - 1) / pageItems : 1;
    if (totalPages > 1) {
      const int currentVisualPage = (pageItems > 0) ? (selectedCategory / pageItems) + 1 : 1;
      char indicator[24];
      snprintf(indicator, sizeof(indicator), "%d / %d", currentVisualPage, totalPages);
      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int indStripTop = pageHeight - metrics.buttonHintsHeight - categoryPageIndicatorH;
      const int indY = indStripTop + (categoryPageIndicatorH - titleLineHeight) / 2;
      const int indW = renderer.getTextWidth(SMALL_FONT_ID, indicator);
      renderer.drawText(SMALL_FONT_ID, (pageWidth - indW) / 2, indY, indicator, true);
    }

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
    // Anchor "Downloading..." just below the cover (if we have one). Falls
    // back to the original centred layout when no cover was pre-fetched.
    int statusY = pageHeight / 2 - 40;
    if (downloadedCoverPath[0] != '\0') {
      FsFile coverFile;
      if (Storage.openFileForRead("BFB", downloadedCoverPath, coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getHeight() > 0) {
          const int coverH = metrics.homeCoverHeight;
          const int coverW = static_cast<int>(static_cast<float>(coverH) * static_cast<float>(bitmap.getWidth()) /
                                              static_cast<float>(bitmap.getHeight()));
          constexpr int coverTextGap = 20;
          // Centre the cover + (status, title, progress, percent) stack around pageHeight/2.
          constexpr int textBlockHeight = 90;  // status(0) + title(30) + bar(20) + percent(40)
          const int coverY = pageHeight / 2 - (coverH + coverTextGap + textBlockHeight) / 2;
          renderer.drawBitmap(bitmap, (pageWidth - coverW) / 2, coverY, coverW, coverH, 0.0f);
          statusY = coverY + coverH + coverTextGap;
        }
        coverFile.close();
      }
    }

    // Use the phase label set by startDownload (Connecting / Downloading /
    // Saving). Falls back to the generic STR_DOWNLOADING if the buffer is
    // somehow blank — should never happen in practice.
    const char* status = (downloadStatus[0] != '\0') ? downloadStatus : tr(STR_DOWNLOADING);
    renderer.drawCenteredText(UI_10_FONT_ID, statusY, status, true, EpdFontFamily::BOLD);
    const int maxWidth = pageWidth - 40;
    auto title = renderer.truncatedText(UI_10_FONT_ID, downloadTitle, maxWidth);
    renderer.drawCenteredText(UI_10_FONT_ID, statusY + 30, title.c_str());

    if (downloadTotal > 0) {
      // Show progress bar with percentage
      constexpr int barX = 50;
      constexpr int barHeight = 20;
      const int barWidth = pageWidth - 100;
      const int barY = statusY + 60;
      GUI.drawProgressBar(renderer, Rect{barX, barY, barWidth, barHeight}, downloadProgress, downloadTotal);

      // Show percentage below progress bar
      const int percent = static_cast<int>((static_cast<uint64_t>(downloadProgress) * 100) / downloadTotal);
      char percentText[16];
      snprintf(percentText, sizeof(percentText), "%d%%", percent);
      renderer.drawCenteredText(UI_10_FONT_ID, barY + 30, percentText);
    } else if (downloadProgress > 0) {
      // Show downloaded bytes when total size is unknown (no Content-Length header)
      char progressText[64];
      if (downloadProgress >= 1024 * 1024) {
        snprintf(progressText, sizeof(progressText), "%.1f MB downloaded...", downloadProgress / (1024.0f * 1024.0f));
      } else if (downloadProgress >= 1024) {
        snprintf(progressText, sizeof(progressText), "%.1f KB downloaded...", downloadProgress / 1024.0f);
      } else {
        snprintf(progressText, sizeof(progressText), "%u bytes downloaded...", downloadProgress);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, statusY + 60, progressText);
    }
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
  // RecentBooksActivity), plus a "current / total(+?)" page indicator strip just
  // above the button hints (matches LibraryActivity's pagination footer).
  constexpr int pageIndicatorHeight = 30;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - pageIndicatorHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, searchResult.count, selectedIndex,
      [this](int index) -> std::string { return std::string(searchResult.books[index].title); },
      [this](int index) -> std::string { return std::string(searchResult.books[index].authors); },
      [this](int index) { return downloadedFlags[index] ? UIIcon::Check : UIIcon::BookFusion; }, nullptr, false);

  // Overlay a strike-through on rows whose book isn't an EPUB so the user can
  // see the book exists but at a glance knows it can't be opened here. We
  // replicate drawList's internal pagination math (BaseTheme/LyraTheme line
  // layout: title at itemY+7, subtitle at itemY+30) so the lines land on the
  // rows that were just drawn. If the theme ever changes those offsets this
  // overlay drifts and is the only thing to update.
  {
    const int rowHeight = metrics.listWithSubtitleRowHeight;
    const int pageItems = (rowHeight > 0) ? contentHeight / rowHeight : 0;
    if (pageItems > 0) {
      const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
      const int titleStrikeY = 7 + renderer.getLineHeight(UI_10_FONT_ID) / 2;
      const int subtitleStrikeY = 30 + renderer.getLineHeight(SMALL_FONT_ID) / 2;
      const int strikeLeft = metrics.contentSidePadding + 8;
      const int strikeRight = pageWidth - metrics.contentSidePadding - 8;
      for (int i = pageStartIndex; i < searchResult.count && i < pageStartIndex + pageItems; ++i) {
        if (bookFusionFormatIsEpub(searchResult.books[i])) continue;
        const int itemY = contentTop + (i % pageItems) * rowHeight;
        renderer.drawLine(strikeLeft, itemY + titleStrikeY, strikeRight, itemY + titleStrikeY, true);
        renderer.drawLine(strikeLeft, itemY + subtitleStrikeY, strikeRight, itemY + subtitleStrikeY, true);
      }
    }
  }

  // Prefer the exact page count when BookFusion returned a `Total-Count`
  // response header (we read it into searchResult.totalCount in the client).
  // If the header is missing for some reason, fall back to the `+` suffix so
  // the user at least knows more pages exist.
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
  const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int indStripTop = pageHeight - metrics.buttonHintsHeight - pageIndicatorHeight;
  const int indY = indStripTop + (pageIndicatorHeight - titleLineHeight) / 2;
  const int indW = renderer.getTextWidth(SMALL_FONT_ID, indicator);
  renderer.drawText(SMALL_FONT_ID, (pageWidth - indW) / 2, indY, indicator, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
