#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "BookFusionSyncClient.h"
#include "network/HttpDownloader.h"

class Epub;
class GfxRenderer;

// The BookFusion download pipeline, shared by the on-device library browser
// (BookFusionBrowserActivity) and the web UI (CrossPointWebServerBookFusion).
// Everything here is free of UI: callers report progress through Hooks.
//
// Memory: the two callers run with very different heaps.
//  - On-device, the framebuffer is in use and the heap is tight, so every TLS
//    session borrows the framebuffer (TlsFramebufferBorrow) and the JPEG and
//    inflate scratch lease it in turn. Pass the renderer in Hooks.
//  - In the web server, the framebuffer has already been released to the heap
//    (CrossPointWebServerActivity::startWebServer) and the serving loop holds
//    the render lock, so a borrow or lease would deadlock on that
//    non-recursive lock. Leave Hooks::renderer null: work runs on the heap and
//    the BookFusion keep-alive connection is closed after each call instead.
namespace BookFusionDownload {

// Book-size threshold above which the device asks before downloading.
// Image-heavy EPUBs strain the C3's RAM; size is a good-enough proxy.
constexpr uint32_t LARGE_BOOK_WARN_BYTES = 10u * 1024 * 1024;  // 10 MB

// Pre-signed S3 URLs can run past 2000 characters.
constexpr size_t DOWNLOAD_URL_LEN = 4096;

// Fetch-and-convert attempts for the API cover before the EPUB transfer.
constexpr int MAX_COVER_ATTEMPTS = 3;

// Only EPUBs can be opened here; BookFusion also serves PDF and audio.
bool formatIsEpub(const BookFusionBook& book);
inline bool isLarge(const BookFusionBook& book) { return book.downloadSize >= LARGE_BOOK_WARN_BYTES; }

// Laid out by SETTINGS.bookfusionFileLayout ("/Title - Author.ext", "/Title.ext"
// or "/Author/Title.ext"), sanitized.
std::string expectedFilename(const BookFusionBook& book);
// Where the book is on the card (following a rename or move, found by its
// BookFusion id), or where a download will put it. `present` reports whether
// the file exists.
std::string localFilename(const BookFusionBook& book, bool& present);

// User-facing sentence for a failed download, with the resume hint when a
// .part file survived and the failure-log hint.
void describeFailure(HttpDownloader::DownloadError err, int httpStatus, bool partialKept, char* out, size_t outLen);

// Caller callbacks. All optional; ctx is passed back unchanged.
struct Hooks {
  void* ctx = nullptr;
  // Non-null on-device: lend the framebuffer to TLS and the decoders. Null in
  // the web server (see the header comment).
  GfxRenderer* renderer = nullptr;
  // A borrow or lease just overwrote the framebuffer. Called with the render
  // lock held, so the caller can mark its next paint as a full one.
  void (*framebufferScrambled)(void* ctx) = nullptr;
  // The EPUB transfer is about to start (no lock held). `resuming` is true
  // when a .part file from an earlier attempt will be continued.
  void (*transferStarting)(void* ctx, bool resuming) = nullptr;
  // A transfer attempt failed and another follows (no lock held).
  void (*retrying)(void* ctx) = nullptr;
  // Bytes received during the transfer (no lock held).
  void (*progress)(void* ctx, size_t downloaded, size_t total) = nullptr;
};

// Opens this download's section in the SD failure log (DownloadFailureLog).
void logStart(const BookFusionBook& book, const std::string& filename, bool resuming);

// One attempt at the API cover: fetch it into the book's cache dir, then build
// the thumbnails and sleep covers. outThumbPath receives the thumb at
// coverHeight. Returns false on either failure.
bool fetchCover(const BookFusionBook& book, const Epub& epub, int coverHeight, char* outThumbPath,
                size_t outThumbPathLen, const Hooks& hooks);
// fetchCover up to MAX_COVER_ATTEMPTS times, logging the outcome. Runs before
// the EPUB transfer so the cover is on the card first; the API cover is the
// only one trusted (BookFusion EPUBs carry broken embedded covers).
bool prefetchCover(const BookFusionBook& book, const Epub& epub, int coverHeight, char* outThumbPath,
                   size_t outThumbPathLen, const Hooks& hooks);

// Outcome of downloadEpub.
struct TransferResult {
  // OK only when getDownloadUrl succeeded on every attempt that ran; anything
  // else aborted the transfer and dlResult is meaningless.
  BookFusionSyncClient::Error urlError = BookFusionSyncClient::NETWORK_ERROR;
  HttpDownloader::DownloadError dlResult = HttpDownloader::HTTP_ERROR;
};

// Fetch a fresh pre-signed URL and stream the EPUB into partPath, up to three
// attempts (resuming the .part), then check it is a complete ZIP and rename it
// to filename. urlBuf (DOWNLOAD_URL_LEN bytes) is caller-owned scratch: the
// activity keeps it as a member, the web handler on the heap, rather than 4KB
// on the stack here.
TransferResult downloadEpub(const BookFusionBook& book, const std::string& partPath, const std::string& filename,
                            char* urlBuf, size_t urlBufLen, const Hooks& hooks);

// Everything after a successful transfer: sidecars, metadata cache, recent
// books entry, BookFusion organisational metadata, a last cover attempt if
// coverOk is false, and the user's BookFusion reading position. coverOk and
// outThumbPath are updated if the late cover attempt succeeds. Returns whether
// the EPUB metadata loaded; the book is on the card either way.
bool finishDownload(const BookFusionBook& book, const std::string& filename, Epub& epub, int coverHeight, bool& coverOk,
                    char* outThumbPath, size_t outThumbPathLen, const Hooks& hooks);

}  // namespace BookFusionDownload
