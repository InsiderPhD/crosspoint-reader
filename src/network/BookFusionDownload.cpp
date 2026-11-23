#include "BookFusionDownload.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>

#include "BookFusionBookIdStore.h"
#include "BookFusionMetaStore.h"
#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "activities/RenderLock.h"
#include "activities/home/LibraryActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "network/BookFusionCoverCache.h"
#include "util/StringUtils.h"
#include "util/ZipFileCheck.h"

namespace BookFusionDownload {
namespace {

constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;

void notifyScrambled(const Hooks& hooks) {
  if (hooks.framebufferScrambled) hooks.framebufferScrambled(hooks.ctx);
}

// One TLS session (or run of calls on one connection). On-device it borrows
// the framebuffer for the session's buffers; the borrow's destructor closes
// the BookFusion keep-alive connection before the arena goes away. In the web
// server there is nothing to borrow, so this only closes that connection,
// returning the TLS context's heap between calls.
class TlsScope {
 public:
  explicit TlsScope(const Hooks& hooks) : hooks(hooks) {
    if (hooks.renderer) borrow.emplace(*hooks.renderer);
  }
  ~TlsScope() {
    if (borrow) {
      notifyScrambled(hooks);  // still under the borrow's render lock
    } else {
      BookFusionSyncClient::closeConnection();
    }
  }
  TlsScope(const TlsScope&) = delete;
  TlsScope& operator=(const TlsScope&) = delete;

 private:
  const Hooks& hooks;
  std::optional<TlsFramebufferBorrow> borrow;
};

// Lends the framebuffer as the JPEG decoder's scratch, on-device only. Without
// a lease the converter falls back to malloc, which the web server's heap (the
// released framebuffer included) can serve.
class JpegScope {
 public:
  explicit JpegScope(const Hooks& hooks) : hooks(hooks) {
    if (!hooks.renderer) return;
    lock.emplace();
    lease.emplace(hooks.renderer->getFrameBuffer(), hooks.renderer->getBufferSize());
  }
  ~JpegScope() {
    if (lease) notifyScrambled(hooks);
  }
  JpegScope(const JpegScope&) = delete;
  JpegScope& operator=(const JpegScope&) = delete;

 private:
  const Hooks& hooks;
  std::optional<RenderLock> lock;  // declared first: taken before the lease, released after
  std::optional<JpegScratchLease> lease;
};

// Same for the 32KB inflate dictionary the metadata build needs.
class InflateScope {
 public:
  explicit InflateScope(const Hooks& hooks) : hooks(hooks) {
    if (!hooks.renderer) return;
    lock.emplace();
    lease.emplace(hooks.renderer->getFrameBuffer(), hooks.renderer->getBufferSize());
  }
  ~InflateScope() {
    if (lease) notifyScrambled(hooks);
  }
  InflateScope(const InflateScope&) = delete;
  InflateScope& operator=(const InflateScope&) = delete;

 private:
  const Hooks& hooks;
  std::optional<RenderLock> lock;
  std::optional<InflateScratchLease> lease;
};

// Writes the BookFusion reading position into progress.bin so the book opens
// where the user left off on another device. Best effort.
void syncRemotePosition(const BookFusionBook& book, const std::string& filename, Epub& epub, const Hooks& hooks) {
  BookFusionPosition remotePos{};
  auto syncErr = BookFusionSyncClient::NETWORK_ERROR;
  {
    TlsScope tls(hooks);
    syncErr = BookFusionSyncClient::getProgress(book.id, remotePos);
  }
  if (syncErr != BookFusionSyncClient::OK) {
    if (syncErr != BookFusionSyncClient::NOT_FOUND) {
      LOG_DBG("BFB", "BookFusion position sync skipped: %s", BookFusionSyncClient::errorString(syncErr));
    }
    return;
  }
  if (remotePos.percentage <= 0.0f) return;

  const int spineCount = epub.getSpineItemsCount();
  if (remotePos.chapterIndex < 0 || remotePos.chapterIndex >= spineCount) return;

  // progress.bin format (EpubReaderActivity.cpp:111-128):
  //   [0..1] spineIndex (LE uint16)
  //   [2..3] pageNumber (LE uint16)  — 0 = start of chapter; the per-chapter
  //                                    page count isn't known until the
  //                                    reader renders the spine item.
  FsFile progressFile;
  if (!Storage.openFileForWrite("BFB", epub.getCachePath() + "/progress.bin", progressFile)) return;
  const uint16_t spineIndex = static_cast<uint16_t>(remotePos.chapterIndex);
  uint8_t data[4] = {static_cast<uint8_t>(spineIndex & 0xFF), static_cast<uint8_t>((spineIndex >> 8) & 0xFF), 0, 0};
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
  LOG_DBG("BFB", "Synced BookFusion position: chapter %d, %d%% (BookFusion authoritative)", remotePos.chapterIndex,
          progressPercent);
}

}  // namespace

bool formatIsEpub(const BookFusionBook& book) {
  if (book.format[0] == '\0') return true;  // Default in the parser is "epub".
  return strcasecmp(book.format, "epub") == 0;
}

namespace {

// Lower-cased format ("epub", "pdf"…), the file extension.
void formatExtension(const BookFusionBook& book, char* ext, size_t len) {
  strlcpy(ext, "epub", len);
  if (book.format[0] == '\0') return;
  size_t i = 0;
  for (; i < len - 1 && book.format[i] != '\0'; i++) {
    ext[i] = static_cast<char>(tolower(static_cast<unsigned char>(book.format[i])));
  }
  ext[i] = '\0';
}

// book.authors is ", "-joined; a folder is named after the first one only.
std::string firstAuthor(const BookFusionBook& book) {
  const char* comma = strstr(book.authors, ", ");
  return comma ? std::string(book.authors, comma - book.authors) : std::string(book.authors);
}

std::string filenameForLayout(const BookFusionBook& book, const uint8_t folder, const uint8_t fileName,
                              const char* suffix) {
  char ext[8];
  formatExtension(book, ext, sizeof(ext));
  const bool hasAuthor = book.authors[0] != '\0';
  std::string dir = "/";
  if (folder == CrossPointSettings::BF_FOLDER_BOOKFUSION || folder == CrossPointSettings::BF_FOLDER_BOOKFUSION_AUTHOR) {
    dir += "BookFusion/";
  }
  // No author: the book goes one level up rather than into an "Unknown" folder.
  if ((folder == CrossPointSettings::BF_FOLDER_AUTHOR || folder == CrossPointSettings::BF_FOLDER_BOOKFUSION_AUTHOR) &&
      hasAuthor) {
    dir += StringUtils::sanitizeFilename(firstAuthor(book)) + "/";
  }
  std::string baseName = book.title;
  if (fileName == CrossPointSettings::BF_NAME_TITLE_AUTHOR && hasAuthor) {
    baseName += " - ";
    baseName += book.authors;
  }
  return dir + StringUtils::sanitizeFilename(baseName) + suffix + "." + ext;
}

// A file at that path that is linked to a different BookFusion book: two
// books with the same name (likely under the Title layout). Never replace it.
bool linkedToOtherBook(const std::string& path, const uint32_t bookId) {
  const uint32_t linked = BookFusionBookIdStore::loadBookId(path.c_str());
  return linked != 0 && linked != bookId;
}

}  // namespace

std::string expectedFilename(const BookFusionBook& book) {
  return filenameForLayout(book, SETTINGS.bookfusionFolder, SETTINGS.bookfusionFileName, "");
}

std::string localFilename(const BookFusionBook& book, bool& present) {
  const uint8_t folder = SETTINGS.bookfusionFolder;
  const uint8_t fileName = SETTINGS.bookfusionFileName;
  std::string path = filenameForLayout(book, folder, fileName, "");
  if (linkedToOtherBook(path, book.id)) {
    // Disambiguate with the id: "/Title (12345).epub".
    char suffix[16];
    snprintf(suffix, sizeof(suffix), " (%lu)", static_cast<unsigned long>(book.id));
    path = filenameForLayout(book, folder, fileName, suffix);
  }
  present = Storage.exists(path.c_str());
  if (present) return path;

  // Downloaded under another folder / file name before the settings changed:
  // keep using that copy rather than adding a second one under the new name.
  for (uint8_t f = 0; f < CrossPointSettings::BOOKFUSION_FOLDER_COUNT; f++) {
    for (uint8_t n = 0; n < CrossPointSettings::BOOKFUSION_FILE_NAME_COUNT; n++) {
      if (f == folder && n == fileName) continue;
      std::string other = filenameForLayout(book, f, n, "");
      if (Storage.exists(other.c_str()) && !linkedToOtherBook(other, book.id)) {
        present = true;
        return other;
      }
    }
  }

  std::string moved;
  // The id check guards against a stale entry: the moved copy deleted and some
  // other file since given its name.
  if (BookFusionBookIdStore::loadMovedPath(book.id, moved) && Storage.exists(moved.c_str()) &&
      BookFusionBookIdStore::loadBookId(moved.c_str()) == book.id) {
    present = true;
    return moved;
  }
  return path;
}

void describeFailure(HttpDownloader::DownloadError err, int httpStatus, bool partialKept, char* out, size_t outLen) {
  char reason[128];
  switch (err) {
    case HttpDownloader::CONNECT_ERROR:
      strlcpy(reason, tr(STR_DL_REASON_CONNECT), sizeof(reason));
      break;
    case HttpDownloader::HTTP_STATUS_ERROR:
      if (httpStatus == 401 || httpStatus == 403 || httpStatus == 416) {
        snprintf(reason, sizeof(reason), tr(STR_DL_REASON_REJECTED_FORMAT), httpStatus);
      } else if (httpStatus == 404) {
        strlcpy(reason, tr(STR_DL_REASON_NOT_FOUND), sizeof(reason));
      } else if (httpStatus >= 500) {
        snprintf(reason, sizeof(reason), tr(STR_DL_REASON_SERVER_FORMAT), httpStatus);
      } else {
        snprintf(reason, sizeof(reason), tr(STR_DL_REASON_HTTP_FORMAT), httpStatus);
      }
      break;
    case HttpDownloader::NO_DATA_ERROR:
      strlcpy(reason, tr(STR_DL_REASON_NO_DATA), sizeof(reason));
      break;
    case HttpDownloader::TRUNCATED_ERROR:
      strlcpy(reason, tr(STR_DL_REASON_TRUNCATED), sizeof(reason));
      break;
    case HttpDownloader::FILE_ERROR:
      strlcpy(reason, tr(STR_DL_REASON_SD), sizeof(reason));
      break;
    case HttpDownloader::ABORTED:
    case HttpDownloader::HTTP_ERROR:
    case HttpDownloader::OK:
    default:
      reason[0] = '\0';
      break;
  }
  if (reason[0] == '\0') {
    strlcpy(out, tr(STR_DOWNLOAD_FAILED), outLen);
  } else {
    snprintf(out, outLen, "%s: %s", tr(STR_DOWNLOAD_FAILED), reason);
  }
  if (partialKept) {
    const size_t used = strlen(out);
    if (used + 1 < outLen) {
      snprintf(out + used, outLen - used, " %s", tr(STR_DL_RESUME_HINT));
    }
  }
}

bool fetchCover(const BookFusionBook& book, const Epub& epub, const int coverHeight, char* outThumbPath,
                const size_t outThumbPathLen, const Hooks& hooks) {
  // Two phases that each lend the idle framebuffer (on-device) to the
  // subsystem that needs it: the TLS borrow for the CDN fetch (an un-borrowed
  // fetch OOMs silently — the original no-cover bug), then a JPEG lease for
  // the conversions (the ~20KB decoder + MCU buffer OOM otherwise). They
  // never alias: the fetch's TLS client is destroyed before its borrow ends.
  bool fetched = false;
  {
    TlsScope tls(hooks);
    fetched = BookFusionCoverCache::download(book.coverUrl, epub, BookFusionCoverCache::kCoverFetchWidth,
                                             BookFusionCoverCache::kCoverFetchHeight);
  }
  if (!fetched) {
    LOG_ERR("BFB", "API cover fetch failed for book_id=%lu", static_cast<unsigned long>(book.id));
    return false;
  }
  bool converted = false;
  {
    JpegScope jpeg(hooks);
    converted = BookFusionCoverCache::convert(epub, coverHeight, outThumbPath, outThumbPathLen);
  }
  if (!converted) {
    LOG_ERR("BFB", "API cover convert failed for book_id=%lu", static_cast<unsigned long>(book.id));
  }
  return converted;
}

bool prefetchCover(const BookFusionBook& book, const Epub& epub, const int coverHeight, char* outThumbPath,
                   const size_t outThumbPathLen, const Hooks& hooks) {
  bool ok = false;
  for (int attempt = 1; attempt <= MAX_COVER_ATTEMPTS && !ok; attempt++) {
    ok = fetchCover(book, epub, coverHeight, outThumbPath, outThumbPathLen, hooks);
    if (!ok) LOG_ERR("BFB", "API cover attempt %d/%d failed", attempt, MAX_COVER_ATTEMPTS);
  }
  if (ok) LOG_DBG("BFB", "Pre-fetched BookFusion API cover before EPUB download");
  return ok;
}

TransferResult downloadEpub(const BookFusionBook& book, const std::string& partPath, const std::string& filename,
                            char* urlBuf, const size_t urlBufLen, const Hooks& hooks) {
  TransferResult result;
  // The Author/Title layout puts the book in a folder that may not exist yet.
  const size_t slash = filename.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = filename.substr(0, slash);
    if (!Storage.ensureDirectoryExists(dir.c_str())) LOG_ERR("BFB", "Could not create %s", dir.c_str());
  }
  // Retry a couple of times: BookFusion's pre-signed URLs sometimes drop
  // mid-transfer and hand back a truncated EPUB (caught by the expectedSize
  // cross-check in downloadToFile). The pre-signed URL is fetched fresh each
  // attempt because it can expire between tries.
  for (int attempt = 1; attempt <= MAX_DOWNLOAD_ATTEMPTS; attempt++) {
    {
      TlsScope tls(hooks);
      result.urlError = BookFusionSyncClient::getDownloadUrl(book.id, urlBuf, urlBufLen);
    }
    if (result.urlError != BookFusionSyncClient::OK) return result;

    if (hooks.transferStarting) hooks.transferStarting(hooks.ctx, Storage.exists(partPath.c_str()));

    {
      // On-device the screen is frozen on a pre-painted card for the whole
      // transfer: the framebuffer is the CDN session's arena. (The
      // downloader's own client is destroyed — and its arena memory freed —
      // inside downloadToFile, before the borrow ends.)
      TlsScope tls(hooks);
      size_t lastLoggedMB = 0;
      result.dlResult = HttpDownloader::downloadToFile(
          urlBuf, partPath,
          [&lastLoggedMB, &hooks](const size_t downloaded, const size_t total) {
            if (hooks.progress) hooks.progress(hooks.ctx, downloaded, total);
            // Serial heartbeat so a long transfer is observable on a frozen screen.
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
          true, static_cast<size_t>(book.downloadSize), /*cancelFlag=*/nullptr, /*username=*/"", /*password=*/"",
          /*headers=*/{}, /*resumePartial=*/true);
    }

    if (result.dlResult == HttpDownloader::OK) break;

    LOG_ERR("BFB", "Download attempt %d/%d failed for book_id=%lu (err=%d, http=%d)", attempt, MAX_DOWNLOAD_ATTEMPTS,
            static_cast<unsigned long>(book.id), static_cast<int>(result.dlResult), HttpDownloader::lastHttpStatus());

    // An SD write failure (card full or pulled) will not fix itself in the
    // next ten seconds; retrying would just burn the same bandwidth twice more.
    if (result.dlResult == HttpDownloader::FILE_ERROR) break;

    // A dropped transfer leaves its bytes in the .part file, so the next
    // attempt (with a fresh pre-signed URL) continues from there.
    if (attempt < MAX_DOWNLOAD_ATTEMPTS && hooks.retrying) hooks.retrying(hooks.ctx);
  }

  // The transport says the body is complete; make sure the FILE is. An EPUB is
  // a ZIP, and a ZIP without its end-of-central-directory record is truncated
  // whatever the server claimed. A failed check deletes the .part: BookFusion's
  // endpoint ignores Range requests, so there is nothing to resume into.
  if (result.dlResult == HttpDownloader::OK && !ZipFileCheck::looksComplete(partPath.c_str())) {
    LOG_ERR("BFB", "%s is not a complete ZIP; discarding", partPath.c_str());
    Storage.remove(partPath.c_str());
    result.dlResult = HttpDownloader::TRUNCATED_ERROR;
  }

  if (result.dlResult == HttpDownloader::OK && !Storage.rename(partPath.c_str(), filename.c_str())) {
    LOG_ERR("BFB", "Rename %s -> %s failed", partPath.c_str(), filename.c_str());
    Storage.remove(partPath.c_str());
    result.dlResult = HttpDownloader::FILE_ERROR;
  }

  return result;
}

bool finishDownload(const BookFusionBook& book, const std::string& filename, Epub& epub, const int coverHeight,
                    bool& coverOk, char* outThumbPath, const size_t outThumbPathLen, const Hooks& hooks) {
  // Save sidecar so BookFusionSyncActivity can find the book_id for this file.
  BookFusionBookIdStore::saveBookId(filename.c_str(), book.id);
  // Lets "Regenerate cover" fetch it directly instead of searching the library.
  BookFusionBookIdStore::saveCoverUrl(filename.c_str(), book.coverUrl);

  // Build metadata. The caller ran clearCache before the transfer, so any
  // pre-existing cache for this filename is gone; epub.load(true) rebuilds it
  // from the freshly-downloaded EPUB. The pre-fetched cover lives at
  // /thumb_<H>.bmp inside the cache dir, which load() does not touch.
  //
  // The metadata build streams the EPUB zip through a 32KB inflate dictionary
  // — the single contiguous allocation a fragmented heap cannot serve
  // (observed on-device: largest block 32756, 12 bytes short) — so on-device
  // it leases the framebuffer, same as section builds do (Section.cpp).
  LOG_DBG("BFB", "Loading EPUB metadata for recent-books entry");
  bool loadSuccess = false;
  {
    InflateScope inflate(hooks);
    loadSuccess = epub.load(true, true);  // buildIfMissing=true, skipLoadingCss=true (we only need metadata)
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

    // No EPUB-cover fallback: BookFusion-served EPUBs frequently carry broken
    // or unreliable cover images, so the only cover we trust is the API one.
    // If the pre-download attempts all failed, this is the last chance: WiFi
    // is still up, the heavy transfer is done, and the heap is at its calmest.
    // Only if THIS also fails does the cover stay unset (text-only popup,
    // library placeholder — never a bad EPUB-derived cover).
    if (!coverOk) coverOk = fetchCover(book, epub, coverHeight, outThumbPath, outThumbPathLen, hooks);
    if (!coverOk) {
      LOG_ERR("BFB", "BookFusion API cover unavailable after all attempts; no EPUB fallback (text-only)");
    }

    syncRemotePosition(book, filename, epub, hooks);
  }

  LOG_DBG("BFB", "Download complete, cache cleared and cover regenerated for book_id=%lu",
          static_cast<unsigned long>(book.id));

  // Force the library to re-scan on its next open. The new file lands in the SD
  // root, whose FAT mtime doesn't change on a child add, so the library index's
  // mtime-based validation can't detect it on its own.
  LibraryActivity::invalidateIndexCache();
  return loadSuccess;
}

}  // namespace BookFusionDownload
