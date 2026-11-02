#include "ClippingSync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DevicePolicy.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <InflateReader.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ClippingStore.h"
#include "HighlightOffsetFinder.h"

// Ported from upstream CrossPoint's src/clippings/ClippingSync.cpp (#3589) onto
// this fork's ClippingStore. Wire format is unchanged. Differences:
//   * Our clips have no layout signature. We send a zero signature (upstream
//     lists such clips but doesn't highlight them), and mark clips pulled with a
//     non-zero one as foreignAnchor so we don't highlight them on the wrong words
//     either.
//   * Source offsets are not tracked per word at layout time (that would cost
//     8 bytes per word on every loaded page); resolveContentOffsets() finds them
//     from the clip text in the chapter XHTML instead, before a sync.
//   * Our store keeps at most CLIPPING_TEXT_MAX bytes of text per clip, so longer
//     remote text is cut at a UTF-8 boundary instead of rejected.
//   * Smaller exchange buffer on the C3, see BUFFER_SIZE.

namespace {
// One item per request/response, reused for both directions. Upstream sizes it
// for a 4KB clip JSON-escaped (32KB). On the C3 a 32KB block is not available
// once BLE has run (largest free block ~25KB, see DevicePolicy.h), so use 12KB:
// our own uploads are <= 512 bytes of text (<= ~4KB escaped), and an ordinary
// 4KB clip from an upstream device fits. A response that doesn't fit fails the
// sync with a log line rather than truncating JSON.
#if CROSSPOINT_TIGHT_HEAP
constexpr size_t BUFFER_SIZE = 12 * 1024;
#else
constexpr size_t BUFFER_SIZE = 32 * 1024;
#endif
// Heap left after the buffer for wolfSSL's small temporaries and ArduinoJson.
constexpr uint32_t MIN_FREE_AFTER_BUFFER = 20000;
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;
constexpr unsigned SYNC_VERSION = 2;

bool request(freeink::SecureHttpClient& http, const std::string& url, char* buffer, JsonDocument& doc,
             const bool upload = false) {
  size_t payloadSize = 0;
  if (upload) {
    if (doc.overflowed() || measureJson(doc) >= BUFFER_SIZE) {
      LOG_ERR("CLIP", "Clipping request exceeds buffer");
      return false;
    }
    payloadSize = serializeJson(doc, buffer, BUFFER_SIZE);
  }
  doc.clear();
  if (!http.begin(url)) {
    LOG_ERR("CLIP", "Invalid sync URL");
    return false;
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  http.addHeader("Content-Type", "application/json");
  size_t received = 0;
  const int status = http.sendRequest(upload ? "PUT" : "GET", reinterpret_cast<const uint8_t*>(buffer), payloadSize,
                                      [&](const uint8_t* data, const size_t length) {
                                        if (length >= BUFFER_SIZE - received) {
                                          LOG_ERR("CLIP", "Clipping response exceeds %u-byte buffer",
                                                  static_cast<unsigned>(BUFFER_SIZE));
                                          return false;
                                        }
                                        memcpy(buffer + received, data, length);
                                        received += length;
                                        return true;
                                      });
  if (status != 200 || !http.responseComplete()) {
    LOG_ERR("CLIP", "Clipping sync HTTP %d (complete=%d)", status, http.responseComplete());
    return false;
  }
  buffer[received] = '\0';
  // Mutable input lets ArduinoJson reference the response buffer without duplicating text.
  const auto error = deserializeJson(doc, buffer, received, DeserializationOption::NestingLimit(4));
  if (error) {
    LOG_ERR("CLIP", "Invalid clipping response: %s", error.c_str());
    return false;
  }
  if (upload && doc["accepted"].as<int>() != 1) {
    LOG_ERR("CLIP", "Server did not acknowledge clipping");
    return false;
  }
  return true;
}

void encode(JsonObject item, const Clipping& clipping) {
  item["id"] = clipping.id;
  item["spine"] = clipping.spineIndex;
  item["start_page"] = clipping.startPage;
  item["end_page"] = clipping.endPage;
  item["pages"] = clipping.pageCount;
  item["start_word"] = clipping.startWordIndex;
  item["end_word"] = clipping.endWordIndex;
  item["words"] = clipping.wordCount;
  if (clipping.paragraphIndex != UINT16_MAX) item["para"] = clipping.paragraphIndex;
  item["chapter"] = clipping.chapterTitle;
  item["text"] = clipping.text.c_str();
  item["created_at"] = clipping.timestamp;
  // Both or neither, [start, end) in chapter visible codepoints.
  if (clipping.startOffset != UINT32_MAX && clipping.endOffset != UINT32_MAX) {
    item["start_offset"] = clipping.startOffset;
    item["end_offset"] = clipping.endOffset;
  }
  // No layout signature in this firmware; zero tells upstream not to trust the
  // page/word anchors after a relayout.
  item["layout_signature"] = 0;
}

bool decode(JsonObjectConst item, Clipping& clipping, bool& deleted) {
  const JsonString id = item["id"].as<JsonString>();
  if (id.isNull() || id.size() == 0 || id.size() >= sizeof(clipping.id)) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (id.c_str()[i] < '!' || id.c_str()[i] > '~') return false;
  }
  memcpy(clipping.id, id.c_str(), id.size());
  clipping.id[id.size()] = '\0';
  if (!item["deleted"].is<unsigned>() || item["deleted"].as<unsigned>() > 1) return false;
  deleted = item["deleted"].as<unsigned>() == 1;
  if (deleted) return true;
  if (!item["revision"].is<uint64_t>() || !item["spine"].is<uint16_t>() || !item["start_page"].is<uint16_t>() ||
      !item["end_page"].is<uint16_t>() || !item["pages"].is<uint16_t>() || !item["start_word"].is<uint16_t>() ||
      !item["end_word"].is<uint16_t>() || !item["words"].is<uint16_t>() || !item["created_at"].is<uint32_t>() ||
      !item["layout_signature"].is<uint32_t>())
    return false;
  if (!item["para"].isNull() && !item["para"].is<uint16_t>()) return false;
  clipping.startOffset = UINT32_MAX;
  clipping.endOffset = UINT32_MAX;
  clipping.offsetsResolved = false;
  if (!item["start_offset"].isNull() || !item["end_offset"].isNull()) {
    if (!item["start_offset"].is<uint32_t>() || !item["end_offset"].is<uint32_t>()) return false;
    clipping.startOffset = item["start_offset"];
    clipping.endOffset = item["end_offset"];
    clipping.offsetsResolved = true;
  }
  const JsonString quote = item["text"].as<JsonString>();
  if (quote.isNull() || quote.size() == 0 || strlen(quote.c_str()) != quote.size()) return false;
  clipping.spineIndex = item["spine"];
  clipping.startPage = item["start_page"];
  clipping.endPage = item["end_page"];
  clipping.pageCount = item["pages"];
  clipping.startWordIndex = item["start_word"];
  clipping.endWordIndex = item["end_word"];
  clipping.wordCount = item["words"];
  clipping.paragraphIndex = item["para"] | UINT16_MAX;
  clipping.timestamp = item["created_at"];
  clipping.syncRevision = item["revision"];
  clipping.pendingUpload = false;
  clipping.foreignAnchor = item["layout_signature"].as<uint32_t>() != 0;
  snprintf(clipping.chapterTitle, sizeof(clipping.chapterTitle), "%s", item["chapter"] | "");
  clipping.chapterTitle[utf8SafeTruncateBuffer(clipping.chapterTitle, strlen(clipping.chapterTitle))] = '\0';
  size_t textLength = quote.size();
  if (textLength > CLIPPING_TEXT_MAX) {
    textLength = static_cast<size_t>(utf8SafeTruncateBuffer(quote.c_str(), static_cast<int>(CLIPPING_TEXT_MAX)));
  }
  clipping.text.assign(quote.c_str(), textLength);
  return true;
}

// Unloads the store however run() returns, if run() was the one that loaded
// it. ClippingStore::unload() also saves anything still dirty.
struct UnloadClippings {
  bool owned = false;
  ~UnloadClippings() {
    if (owned) CLIPPINGS.unload();
  }
};
}  // namespace

bool clippingSync::resolveContentOffsets(GfxRenderer& renderer, const Epub& epub) {
  if (!KOREADER_STORE.getSyncClippings()) return false;
  UnloadClippings unload;
  if (CLIPPINGS.getBookFilePath() != epub.getPath()) {
    if (!CLIPPINGS.loadForBook(epub.getPath(), epub.getTitle(), epub.getAuthor(), "epub")) return false;
    unload.owned = true;
  }
  const auto& clips = CLIPPINGS.getClippings();
  const bool anyPending = std::any_of(clips.begin(), clips.end(),
                                      [](const Clipping& clip) { return !clip.text.empty() && !clip.offsetsResolved; });
  if (!anyPending) return false;

  InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
  bool changed = false;
  for (size_t i = 0; i < clips.size(); ++i) {
    const Clipping& clip = clips[i];
    if (clip.text.empty() || clip.offsetsResolved) continue;
    if (static_cast<int>(clip.spineIndex) >= epub.getSpineItemsCount()) {
      CLIPPINGS.setContentOffsets(i, false, 0, 0);
      changed = true;
      continue;
    }
    const float fraction =
        clip.pageCount > 0 ? (static_cast<float>(clip.startPage) + 0.5f) / static_cast<float>(clip.pageCount) : 0.0f;
    uint32_t start = 0;
    uint32_t end = 0;
    const auto result = HighlightOffsetFinder::find(epub, clip.spineIndex, clip.text, fraction, start, end,
                                                    HighlightOffsetFinder::Unit::Codepoint);
    if (result == HighlightOffsetFinder::Result::Error) continue;  // retried next sync
    CLIPPINGS.setContentOffsets(i, result == HighlightOffsetFinder::Result::Found, start, end);
    changed = true;
  }
  if (changed && !CLIPPINGS.saveToFile()) {
    LOG_ERR("CLIP", "Failed to persist clipping content offsets");
  }
  return scratch.active();
}

bool clippingSync::run(const std::string& bookPath, const std::string& title, const std::string& author,
                       const std::string& documentHash) {
  // Gate before loading files, allocating buffers, or contacting the server.
  if (!KOREADER_STORE.getSyncClippings()) return true;
  if (!KOREADER_STORE.hasCredentials()) return false;
  UnloadClippings unload;
  if (CLIPPINGS.getBookFilePath() != bookPath) {
    if (!CLIPPINGS.loadForBook(bookPath, title, author, "epub")) return false;
    unload.owned = true;
  }

  // Heap, not stack: BUFFER_SIZE is far over the 256-byte stack budget. Freed
  // when run() returns; one allocation per manual sync.
  auto buffer = makeUniqueNoThrow<char[]>(BUFFER_SIZE);
  auto http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!buffer || !http || ESP.getFreeHeap() < MIN_FREE_AFTER_BUFFER) {
    LOG_ERR("CLIP", "Insufficient heap for clipping sync (%u free, %u max alloc)", (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
    return false;
  }
  http->setInsecure();
  http->setTimeout(HTTP_TIMEOUT_MS);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/clippings/" + documentHash;
  JsonDocument doc;
  // Probe the extension before any upload, even when the user explicitly enabled it.
  if (!request(*http, url + "?cursor=0&limit=1&format=reader", buffer.get(), doc) ||
      doc["sync_version"].as<unsigned>() != SYNC_VERSION) {
    LOG_ERR("CLIP", "Clipping sync requires a compatible crosspoint-sync server");
    return false;
  }
  if (!CLIPPINGS.prepareSync()) return false;

  uint32_t deletionOffset = 0;
  char deletedId[CLIPPING_ID_SIZE];
  while (true) {
    if (!CLIPPINGS.nextDeletion(deletionOffset, deletedId)) return false;
    if (!deletedId[0]) break;
    doc.clear();
    JsonObject item = doc["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = deletedId;
    item["deleted"] = 1;
    if (!request(*http, url, buffer.get(), doc, true)) return false;
  }
  if (!CLIPPINGS.finishDeletions()) return false;

  for (const Clipping& clipping : CLIPPINGS.getClippings()) {
    if (!clipping.pendingUpload) continue;
    doc.clear();
    encode(doc["items"].to<JsonArray>().add<JsonObject>(), clipping);
    if (!request(*http, url, buffer.get(), doc, true)) return false;
  }
  if (!CLIPPINGS.finishUploads()) return false;

  // Rescans this book's history from the start on every manual sync, as upstream does.
  uint64_t cursor = 0;
  do {
    if (!request(*http, url + "?limit=1&format=reader&cursor=" + std::to_string(cursor), buffer.get(), doc))
      return false;
    if (!doc["cursor"].is<uint64_t>() || !doc["more"].is<bool>() || !doc["items"].is<JsonArray>()) return false;
    const uint64_t next = doc["cursor"];
    const JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.size() > 1 || next < cursor || (items.size() > 0 && next <= cursor) ||
        (doc["more"].as<bool>() && items.size() == 0)) {
      LOG_ERR("CLIP", "Invalid clipping pagination");
      return false;
    }
    for (const JsonObjectConst item : items) {
      Clipping clipping;
      bool deleted = false;
      if (!decode(item, clipping, deleted)) {
        LOG_ERR("CLIP", "Invalid remote clipping");
        return false;
      }
      if (!CLIPPINGS.applyRemote(clipping, deleted)) return false;
    }
    cursor = next;
  } while (doc["more"].as<bool>());
  return true;
}
