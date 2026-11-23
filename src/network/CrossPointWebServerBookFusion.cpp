// BookFusion library routes: browse the linked account's library from the web
// UI and download books straight to the SD card. The download runs the same
// pipeline as the on-device browser (BookFusionDownload), so the file name,
// sidecars, cover and synced reading position come out identical.
//
// Memory: the web server activity has already released the framebuffer to the
// heap and holds the render lock for every serving pass, so nothing here may
// borrow the framebuffer — BookFusionDownload::Hooks::renderer stays null.
// Each API call is bracketed by suspendTransferServices() and ends with the
// BookFusion keep-alive connection closed, returning the TLS context's heap.
#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookFusionBookIdStore.h"
#include "BookFusionSyncClient.h"
#include "BookFusionTokenStore.h"
#include "CrossPointWebServer.h"
#include "activities/home/LibraryActivity.h"
#include "activities/home/LibraryScan.h"
#include "components/UITheme.h"
#include "html/BookFusionPageHtml.generated.h"
#include "network/BookFusionDownload.h"

namespace {

// The library lists the on-device browser offers (BookFusionBrowserActivity's
// CATEGORIES). Anything else is refused rather than forwarded to the API.
struct ListFilter {
  const char* list;
  const char* sort;
};
constexpr ListFilter LIST_FILTERS[] = {
    {"currently_reading", "last_read_at-desc"},
    {"favorites", nullptr},
    {"planned_to_read", nullptr},
    {"completed", nullptr},
};

const ListFilter* findListFilter(const String& list) {
  for (const auto& filter : LIST_FILTERS) {
    if (list == filter.list) return &filter;
  }
  return nullptr;
}

// A path the link endpoints may touch: an absolute EPUB path with no ".."
// segment, outside the firmware's own folder. Existence is checked by the
// caller where it matters.
bool linkablePath(const String& path) {
  if (path.length() < 2 || path[0] != '/') return false;
  if (path.indexOf("/../") >= 0 || path.endsWith("/..")) return false;
  if (path.startsWith("/.crosspoint")) return false;
  return FsHelpers::hasEpubExtension(path);
}

bool wifiOnline() { return (WiFi.getMode() & WIFI_STA) && WiFi.status() == WL_CONNECTED; }

// HTTP status for an API failure: an expired link is the user's to fix on the
// device, anything else is upstream trouble.
int httpStatusFor(const BookFusionSyncClient::Error err) {
  switch (err) {
    case BookFusionSyncClient::NO_TOKEN:
    case BookFusionSyncClient::AUTH_FAILED:
      return 401;
    case BookFusionSyncClient::NOT_FOUND:
      return 404;
    default:
      return 502;
  }
}

void copyArg(WebServer& server, const char* name, char* out, const size_t outLen) {
  strlcpy(out, server.arg(name).c_str(), outLen);
}

// The download handler's response: newline-delimited JSON, streamed while the
// transfer runs, so the page can draw a progress bar. The reader serves one
// request at a time and is busy for the whole download, so the page cannot
// poll for progress; the progress travels in the response instead. Lines:
//   {"stage":"cover"|"downloading"|"retrying"|"saving"[,"resuming":true]}
//   {"p":<bytes so far>,"t":<total bytes, 0 if unknown>}   (at most ~2/s)
//   {"ok":true,...} | {"ok":false,"error":"..."}            (last line)
// The regular lines also keep a phone from dropping a quiet connection.
// Each line is a few dozen bytes, built on the stack.
struct DownloadStream {
  static constexpr unsigned long PROGRESS_INTERVAL_MS = 500;

  WebServer* server = nullptr;
  unsigned long lastProgress = 0;

  void begin() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/x-ndjson", "");
  }

  void line(const char* json) {
    if (!server->client().connected()) return;
    server->sendContent(json);
    server->sendContent("\n", 1);
  }

  void stage(const char* name, const bool resuming = false) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"stage\":\"%s\"%s}", name, resuming ? ",\"resuming\":true" : "");
    line(buf);
  }

  void progress(const size_t done, const size_t total) {
    const unsigned long now = millis();
    if (now - lastProgress < PROGRESS_INTERVAL_MS) return;
    lastProgress = now;
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"p\":%u,\"t\":%u}", static_cast<unsigned>(done), static_cast<unsigned>(total));
    line(buf);
  }

  void finish(const JsonDocument& doc) {
    if (server->client().connected()) {
      String out;
      serializeJson(doc, out);
      out += '\n';
      server->sendContent(out);
    }
    server->sendContent("", 0);  // end of the chunked response
  }
};

}  // namespace

void CrossPointWebServer::handleBookFusionPage() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", BookFusionPageHtml, sizeof(BookFusionPageHtml));
  LOG_DBG("WEB", "Served BookFusion page");
}

// GET /api/bookfusion/status -> {"linked":bool,"online":bool}
// online is false in hotspot mode: the device has no route to BookFusion.
void CrossPointWebServer::handleBookFusionStatus() const {
  char out[48];
  snprintf(out, sizeof(out), "{\"linked\":%s,\"online\":%s}", BF_TOKEN_STORE.hasToken() ? "true" : "false",
           wifiOnline() ? "true" : "false");
  server->send(200, "application/json", out);
}

// GET /api/bookfusion/shelves -> [{"id":1,"name":"..."},...]
void CrossPointWebServer::handleBookFusionShelves() {
  if (!BF_TOKEN_STORE.hasToken() || !wifiOnline()) {
    server->send(409, "application/json", "{\"error\":\"not linked or offline\"}");
    return;
  }
  // ~3.4 KB: too big for the stack, held only for this request.
  auto shelves = makeUniqueNoThrow<BookFusionBookshelfList>();
  if (!shelves) {
    LOG_ERR("WEB", "OOM: BookFusion shelf list");
    server->send(500, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }

  BookFusionSyncClient::Error err;
  {
    suspendTransferServices();
    ScopedCleanup resume{[this] { resumeTransferServices(); }};
    err = BookFusionSyncClient::searchBookshelves(*shelves);
    BookFusionSyncClient::closeConnection();
  }
  if (err != BookFusionSyncClient::OK) {
    LOG_ERR("WEB", "BookFusion shelves failed: %s", BookFusionSyncClient::errorString(err));
    JsonDocument doc;
    doc["error"] = BookFusionSyncClient::errorString(err);
    String out;
    serializeJson(doc, out);
    server->send(httpStatusFor(err), "application/json", out);
    return;
  }

  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < shelves->count; i++) {
    JsonObject shelf = arr.add<JsonObject>();
    shelf["id"] = shelves->shelves[i].id;
    shelf["name"] = shelves->shelves[i].name;
  }
  String out;
  serializeJson(doc, out);
  server->send(200, "application/json", out);
}

// GET /api/bookfusion/books?page=1&list=<filter>&shelf=<id>&q=<text>
// -> {"page":1,"hasMore":bool,"total":N,"books":[{id,title,authors,format,
//     size,cover,categories,bookshelves,lists,epub,onCard,path?,linked?}]}
// path (where the book is on the card) and linked (it carries this book's
// BookFusion id) are present only when onCard.
// list, shelf and q are alternatives, as on the device: q wins, then shelf.
// Streamed one book at a time to keep the response off the heap.
void CrossPointWebServer::handleBookFusionBooks() {
  if (!BF_TOKEN_STORE.hasToken() || !wifiOnline()) {
    server->send(409, "application/json", "{\"error\":\"not linked or offline\"}");
    return;
  }

  int page = server->arg("page").toInt();
  if (page < 1) page = 1;
  char query[64];
  copyArg(*server, "q", query, sizeof(query));
  const uint32_t shelfId = query[0] == '\0' ? static_cast<uint32_t>(server->arg("shelf").toInt()) : 0;
  const ListFilter* filter = nullptr;
  if (query[0] == '\0' && shelfId == 0 && server->hasArg("list") && server->arg("list").length() > 0) {
    filter = findListFilter(server->arg("list"));
    if (!filter) {
      server->send(400, "application/json", "{\"error\":\"unknown list\"}");
      return;
    }
  }

  // ~8 KB (ten books with cover URLs): heap, held only for this request.
  auto result = makeUniqueNoThrow<BookFusionSearchResult>();
  if (!result) {
    LOG_ERR("WEB", "OOM: BookFusion search result");
    server->send(500, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }

  BookFusionSyncClient::Error err;
  {
    suspendTransferServices();
    ScopedCleanup resume{[this] { resumeTransferServices(); }};
    err =
        BookFusionSyncClient::searchBooks(page, *result, filter ? filter->list : nullptr,
                                          filter ? filter->sort : nullptr, shelfId, query[0] != '\0' ? query : nullptr);
    BookFusionSyncClient::closeConnection();
  }
  if (err != BookFusionSyncClient::OK) {
    LOG_ERR("WEB", "BookFusion search failed: %s", BookFusionSyncClient::errorString(err));
    JsonDocument doc;
    doc["error"] = BookFusionSyncClient::errorString(err);
    String out;
    serializeJson(doc, out);
    server->send(httpStatusFor(err), "application/json", out);
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  char head[80];
  snprintf(head, sizeof(head), "{\"page\":%d,\"hasMore\":%s,\"total\":%d,\"books\":[", page,
           result->hasMore ? "true" : "false", result->totalCount);
  server->sendContent(head);

  String out;
  for (int i = 0; i < result->count; i++) {
    const BookFusionBook& book = result->books[i];
    bool onCard = false;
    const std::string localPath = BookFusionDownload::localFilename(book, onCard);

    JsonDocument doc;
    doc["id"] = book.id;
    doc["title"] = book.title;
    doc["authors"] = book.authors;
    doc["format"] = book.format;
    doc["size"] = book.downloadSize;
    doc["cover"] = book.coverUrl;
    doc["categories"] = book.categories;
    doc["bookshelves"] = book.bookshelves;
    doc["lists"] = book.lists;
    doc["epub"] = BookFusionDownload::formatIsEpub(book);
    doc["onCard"] = onCard;
    if (onCard) {
      doc["path"] = localPath;
      // A file under the download name is not necessarily linked (it may have
      // been sideloaded, or unlinked since).
      doc["linked"] = BookFusionBookIdStore::loadBookId(localPath.c_str()) == book.id;
    }
    // ArduinoJson clears the String it serializes into, so the separator goes
    // out on its own rather than as a prefix of `out`.
    if (i > 0) server->sendContent(",");
    serializeJson(doc, out);
    server->sendContent(out);
  }
  server->sendContent("]}");
  server->sendContent("", 0);
}

// POST /api/bookfusion/download, form fields: id, title, authors, format,
// size, cover, categories, bookshelves, lists — the book as /books returned
// it. The API has no get-by-id endpoint, so the page hands the fields back
// rather than the device searching for the book again.
// -> a DownloadStream (200, NDJSON) ending in
//    {"ok":true,"path":"/Title - Author.epub","cover":bool,"metadata":bool}
//  | {"ok":false,"error":"..."}
// Requests refused before the transfer starts get a plain JSON error with a
// 4xx/5xx status instead. Blocks for the whole transfer.
void CrossPointWebServer::handleBookFusionDownload() {
  if (!BF_TOKEN_STORE.hasToken() || !wifiOnline()) {
    server->send(409, "application/json", "{\"ok\":false,\"error\":\"not linked or offline\"}");
    return;
  }

  // Held for the whole request: ~1 KB, heap so it stays off the stack.
  auto book = makeUniqueNoThrow<BookFusionBook>();
  if (!book) {
    LOG_ERR("WEB", "OOM: BookFusion book");
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
    return;
  }
  book->id = static_cast<uint32_t>(server->arg("id").toInt());
  copyArg(*server, "title", book->title, sizeof(book->title));
  copyArg(*server, "authors", book->authors, sizeof(book->authors));
  copyArg(*server, "format", book->format, sizeof(book->format));
  book->downloadSize = static_cast<uint32_t>(server->arg("size").toInt());
  copyArg(*server, "cover", book->coverUrl, sizeof(book->coverUrl));
  copyArg(*server, "categories", book->categories, sizeof(book->categories));
  copyArg(*server, "bookshelves", book->bookshelves, sizeof(book->bookshelves));
  copyArg(*server, "lists", book->lists, sizeof(book->lists));
  if (book->id == 0 || book->title[0] == '\0') {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"missing id or title\"}");
    return;
  }
  if (!BookFusionDownload::formatIsEpub(*book)) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"only EPUB books can be downloaded\"}");
    return;
  }
  // Pre-signed S3 URLs run past 2000 characters: 4 KB, heap for the same reason.
  auto urlBuf = makeUniqueNoThrow<char[]>(BookFusionDownload::DOWNLOAD_URL_LEN);
  if (!urlBuf) {
    LOG_ERR("WEB", "OOM: BookFusion download URL buffer");
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"out of memory\"}");
    return;
  }
  releaseRequestArguments();  // the form is copied; give its memory to TLS

  suspendTransferServices();
  ScopedCleanup resume{[this] { resumeTransferServices(); }};

  bool alreadyOnCard = false;
  const std::string filename = BookFusionDownload::localFilename(*book, alreadyOnCard);
  const std::string partPath = filename + ".part";
  LOG_INF("WEB", "BookFusion download book_id=%lu -> %s%s", static_cast<unsigned long>(book->id), filename.c_str(),
          alreadyOnCard ? " (replacing)" : "");

  DownloadStream stream;
  stream.server = server.get();
  stream.begin();
  BookFusionDownload::Hooks hooks;  // renderer stays null: see the file header
  hooks.ctx = &stream;
  hooks.transferStarting = [](void* ctx, const bool resuming) {
    static_cast<DownloadStream*>(ctx)->stage("downloading", resuming);
  };
  hooks.retrying = [](void* ctx) { static_cast<DownloadStream*>(ctx)->stage("retrying"); };
  hooks.progress = [](void* ctx, const size_t done, const size_t total) {
    static_cast<DownloadStream*>(ctx)->progress(done, total);
  };

  stream.stage("cover");
  Epub epub(filename, "/.crosspoint");
  epub.clearCache();
  epub.setupCacheDir();
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  char thumbPath[96] = {};
  bool coverOk = BookFusionDownload::prefetchCover(*book, epub, coverHeight, thumbPath, sizeof(thumbPath), hooks);

  const auto transfer = BookFusionDownload::downloadEpub(*book, partPath, filename, urlBuf.get(),
                                                         BookFusionDownload::DOWNLOAD_URL_LEN, hooks);
  urlBuf.reset();

  JsonDocument doc;
  if (transfer.urlError != BookFusionSyncClient::OK) {
    doc["ok"] = false;
    doc["error"] = transfer.urlError == BookFusionSyncClient::NOT_FOUND
                       ? tr(STR_BF_BOOK_UNAVAILABLE)
                       : BookFusionSyncClient::errorString(transfer.urlError);
    stream.finish(doc);
    return;
  }
  if (transfer.dlResult != HttpDownloader::OK) {
    char msg[224];
    BookFusionDownload::describeFailure(transfer.dlResult, HttpDownloader::lastHttpStatus(),
                                        Storage.exists(partPath.c_str()), msg, sizeof(msg));
    doc["ok"] = false;
    doc["error"] = msg;
    stream.finish(doc);
    return;
  }

  stream.stage("saving");
  const bool loaded = BookFusionDownload::finishDownload(*book, filename, epub, coverHeight, coverOk, thumbPath,
                                                         sizeof(thumbPath), hooks);

  doc["ok"] = true;
  doc["path"] = filename;
  doc["cover"] = coverOk;
  doc["metadata"] = loaded;
  stream.finish(doc);
}

// GET /api/bookfusion/local-books -> [{"path":"/Books/x.epub","linked":bool},...]
// Every EPUB on the card, from the library's path cache when it is current
// (LibraryScan::enumerateBooks; a full card walk otherwise). linked means the
// book already has a BookFusion sidecar. Streamed so only the path list itself
// is held — the same list the on-device library holds while browsing.
void CrossPointWebServer::handleBookFusionLocalBooks() const {
  std::vector<std::string> paths;
  LibraryScan::enumerateBooks(paths);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "[");
  JsonDocument doc;
  String out;
  bool first = true;
  for (size_t i = 0; i < paths.size(); i++) {
    const std::string& path = paths[i];
    if (!FsHelpers::hasEpubExtension(path)) continue;
    doc.clear();
    doc["path"] = path;
    // One SD probe per book (the sidecar's existence): keep the watchdog fed.
    doc["linked"] = BookFusionBookIdStore::hasBookId(path.c_str());
    if (!first) server->sendContent(",");  // separately: serializeJson clears `out`
    first = false;
    serializeJson(doc, out);
    server->sendContent(out);
    if ((i & 0x0Fu) == 0x0Fu) esp_task_wdt_reset();
  }
  server->sendContent("]");
  server->sendContent("", 0);
  LOG_DBG("WEB", "Listed %u books for BookFusion linking", static_cast<unsigned>(paths.size()));
}

// POST /api/bookfusion/link, form: path (an EPUB on the card), id (BookFusion
// book id), cover (the book's API cover URL, optional) -> {"ok":true}
// For books that did not come from BookFusion. Writes a fresh sidecar, so the
// book syncs reading position with BookFusion from its next open (taking
// precedence over KOReader sync, as for any BookFusion book). Local only: no
// network call, so it works without the account being reachable.
void CrossPointWebServer::handleBookFusionLink() const {
  const String path = server->arg("path");
  const uint32_t bookId = static_cast<uint32_t>(server->arg("id").toInt());
  if (!linkablePath(path) || bookId == 0) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad path or id\"}");
    return;
  }
  if (!Storage.exists(path.c_str())) {
    server->send(404, "application/json", "{\"ok\":false,\"error\":\"file not found\"}");
    return;
  }
  if (!BookFusionBookIdStore::link(path.c_str(), bookId, server->arg("cover").c_str())) {
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"could not write to the SD card\"}");
    return;
  }
  // The library index caches each book's BookFusion badge.
  LibraryActivity::invalidateIndexCache();
  server->send(200, "application/json", "{\"ok\":true}");
}

// POST /api/bookfusion/unlink, form: path -> {"ok":true}
// Removes the book's BookFusion link (its whole sidecar); the file is kept.
void CrossPointWebServer::handleBookFusionUnlink() const {
  const String path = server->arg("path");
  if (!linkablePath(path)) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"bad path\"}");
    return;
  }
  if (!BookFusionBookIdStore::unlink(path.c_str())) {
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"could not write to the SD card\"}");
    return;
  }
  LibraryActivity::invalidateIndexCache();
  server->send(200, "application/json", "{\"ok\":true}");
}
