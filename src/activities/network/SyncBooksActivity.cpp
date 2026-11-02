#include "SyncBooksActivity.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncStateStore.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <ProgressComparison.h>
#include <ProgressMapper.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/home/LibraryScan.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HardcoverSync.h"
#include "util/TimeUtils.h"
#include "util/WifiTimeSync.h"

namespace {
constexpr char FEED_STATE_PATH[] = "/.crosspoint/kosync_feed.json";
constexpr char CACHE_DIR[] = "/.crosspoint";
// At most this many progress repaints per phase, whatever the library size:
// each is an e-ink refresh.
constexpr int MAX_REPAINTS_PER_PHASE = 40;

// The change-feed cursor belongs to one server, account and document-id
// scheme: any of them changing makes it meaningless, so it is stored with a
// hash of all three and ignored when that no longer matches.
std::string feedKey() {
  MD5Builder md5;
  md5.begin();
  md5.add(KOREADER_STORE.getBaseUrl().c_str());
  md5.add("\n");
  md5.add(KOREADER_STORE.getUsername().c_str());
  md5.add(KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY ? "\nbinary" : "\nfilename");
  md5.calculate();
  return md5.toString().c_str();
}

uint32_t loadFeedCursor() {
  std::string json;
  if (!Storage.exists(FEED_STATE_PATH) || !Storage.readFileToString("SYNCB", FEED_STATE_PATH, 256, json)) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return 0;
  if (feedKey() != (doc["key"] | "")) return 0;
  return doc["cursor"] | 0u;
}

void saveFeedCursor(const uint32_t cursor) {
  if (cursor == loadFeedCursor()) return;  // no card write for an unchanged cursor
  JsonDocument doc;
  doc["key"] = feedKey();
  doc["cursor"] = cursor;
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(FEED_STATE_PATH, json)) LOG_ERR("SYNCB", "Could not save the change-feed cursor");
}

std::string documentIdFor(const std::string& path) {
  return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(path)
             : KOReaderDocumentId::calculate(path);
}

// progress.bin: spine, page (LE uint16) and, in the 6-byte form, the chapter's
// page count. A 4-byte file has no count and cannot be mapped to a percentage.
bool readSavedProgress(const std::string& cachePath, int& spine, int& page, int& pages) {
  FsFile f;
  if (!Storage.openFileForRead("SYNCB", cachePath + "/progress.bin", f)) return false;
  uint8_t data[6] = {};
  const int n = f.read(data, sizeof(data));
  f.close();
  if (n != 6) return false;
  spine = data[0] | (data[1] << 8);
  page = data[2] | (data[3] << 8);
  pages = data[4] | (data[5] << 8);
  return pages > 0 && page < pages && page != UINT16_MAX;
}

int64_t nowIfClockValid() {
  const uint32_t now = TimeUtils::getCurrentValidTimestamp();
  return TimeUtils::isClockValid(now) ? static_cast<int64_t>(now) : 0;
}
}  // namespace

void SyncBooksActivity::onEnter() {
  Activity::onEnter();
  if (!KOREADER_STORE.hasCredentials()) {
    state = State::NoCredentials;
    requestUpdate();
    return;
  }
  startWifi();
}

void SyncBooksActivity::onExit() {
  Activity::onExit();
  books.clear();
  books.shrink_to_fit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(100);
    WiFi.mode(WIFI_OFF);
  }
}

void SyncBooksActivity::startWifi() {
  state = State::Connecting;
  HardcoverSync::preempt();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             failMessage = tr(STR_WIFI_CONN_FAILED);
                             state = State::Failed;
                             requestUpdate();
                             return;
                           }
                           // Glyph cache re-warms on the next paint; TLS wants the heap now.
                           if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
                           WiFi.setSleep(false);  // modem sleep stalls sockets (upstream #3233)
                           state = State::Running;
                         });
}

void SyncBooksActivity::loop() {
  switch (state) {
    case State::Running:
      runSync();
      return;
    case State::Connecting:
      return;
    case State::NoCredentials:
    case State::Done:
    case State::Failed:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        finish();
      }
      return;
  }
}

void SyncBooksActivity::step(const char* newPhase, const int done, const int count) {
  const bool newStage = newPhase != phase;
  phase = newPhase;
  processed = done;
  total = count;
  const int stepSize = count > MAX_REPAINTS_PER_PHASE ? count / MAX_REPAINTS_PER_PHASE : 1;
  if (newStage || done == count || done - lastRepaint >= stepSize) {
    lastRepaint = done;
    requestUpdateAndWait();
  }
}

void SyncBooksActivity::runSync() {
  step(tr(STR_SYNC_CHECKING_BOOKS), 0, 0);
  WifiTimeSync::attemptIfStale();  // sync timestamps need a real clock

  bool ok = collectBooks();
  if (ok && books.empty()) {
    failMessage = tr(STR_SYNC_NO_BOOKS);
  }
  if (ok && !books.empty()) {
    bool feedUsed = false;
    ok = readChangeFeed(feedUsed);
    if (ok && !feedUsed) ok = walkBooks();
    if (ok) ok = uploadChanged();
  }
  LOG_INF("SYNCB", "Done: %u up, %u pulled, %u same, %u conflicts, %u failed (heap %u)", uploaded, pulled, unchanged,
          conflicts, failed, (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    state = ok ? State::Done : State::Failed;
  }
  requestUpdate();
}

bool SyncBooksActivity::collectBooks() {
  books.clear();
  std::vector<std::string> paths;
  LibraryScan::enumerateBooks(paths);
  // One entry per EPUB with a saved position; unread books have nothing to sync.
  books.reserve(paths.size());
  const int count = static_cast<int>(paths.size());
  for (int i = 0; i < count; i++) {
    std::string& path = paths[i];
    if (FsHelpers::hasEpubExtension(path)) {
      const Epub epub(path, CACHE_DIR);
      int spine = 0, page = 0, pages = 0;
      if (readSavedProgress(epub.getCachePath(), spine, page, pages)) {
        const std::string hash = documentIdFor(path);
        if (hash.size() == 32) {
          Book book;
          book.path = std::move(path);
          memcpy(book.hash, hash.c_str(), 33);
          books.push_back(std::move(book));
        }
      }
    }
    vTaskDelay(1);  // binary matching reads the file; never starve the watchdog
    step(tr(STR_SYNC_CHECKING_BOOKS), i + 1, count);
  }
  books.shrink_to_fit();
  LOG_INF("SYNCB", "%u of %d books have a saved position (heap %u)", static_cast<unsigned>(books.size()), count,
          (unsigned)ESP.getFreeHeap());
  return true;
}

bool SyncBooksActivity::readChangeFeed(bool& feedUsed) {
  feedUsed = false;
  if (!KOREADER_STORE.supportsSyncExtensions()) return true;

  uint32_t cursor = loadFeedCursor();
  std::vector<KOReaderProgressChange> changes;
  int pages = 0;
  for (;;) {
    KOReaderSyncClient::ChangesPage page;
    KOReaderSyncClient::Error err;
    {
      TlsFramebufferBorrow borrow(renderer);
      err = KOReaderSyncClient::getProgressChanges(cursor, KOReaderSyncClient::MAX_CHANGES_PAGE, page, changes);
    }
    if (err == KOReaderSyncClient::FEED_UNSUPPORTED) {
      LOG_INF("SYNCB", "No change feed on this server; checking each book");
      return true;
    }
    if (err != KOReaderSyncClient::OK) {
      failMessage = KOReaderSyncClient::errorString(err);
      return false;
    }
    feedUsed = true;
    pages++;
    for (const auto& change : changes) {
      for (auto& book : books) {
        if (book.handled || !change.matches(book.hash)) continue;
        Remote remote;
        remote.percentage = change.percentage;
        remote.progress = change.progress;
        remote.position = change.position;
        remote.timestamp = change.timestamp;
        handleRemote(book, remote);
        break;
      }
    }
    changes.clear();
    cursor = page.cursor;
    // Saved per page: a run stopped half way picks up from here next time,
    // and re-reading an already applied page is harmless.
    saveFeedCursor(cursor);
    step(tr(STR_SYNC_CHECKING_UPDATES), pages, page.more ? pages + 1 : pages);
    if (!page.more || page.count == 0) break;
  }
  return true;
}

bool SyncBooksActivity::walkBooks() {
  const int count = static_cast<int>(books.size());
  for (int i = 0; i < count; i++) {
    Book& book = books[i];
    KOReaderProgress progress;
    KOReaderSyncClient::Error err;
    {
      TlsFramebufferBorrow borrow(renderer);
      err = KOReaderSyncClient::getProgress(book.hash, progress);
    }
    if (err == KOReaderSyncClient::OK) {
      Remote remote;
      remote.percentage = progress.percentage;
      remote.progress = std::move(progress.progress);
      remote.position = std::move(progress.position);
      remote.timestamp = progress.timestamp;
      handleRemote(book, remote);
    } else if (err == KOReaderSyncClient::AUTH_FAILED || err == KOReaderSyncClient::NETWORK_ERROR) {
      // Every later book would fail the same way.
      failMessage = KOReaderSyncClient::errorString(err);
      return false;
    } else if (err != KOReaderSyncClient::NOT_FOUND) {
      LOG_ERR("SYNCB", "Progress lookup failed for %s: %s", book.path.c_str(), KOReaderSyncClient::errorString(err));
      book.handled = true;  // don't overwrite a server position we could not read
      failed++;
    }
    step(tr(STR_SYNC_CHECKING_UPDATES), i + 1, count);
  }
  return true;
}

void SyncBooksActivity::handleRemote(Book& book, const Remote& remote) {
  int64_t lastTs = 0;
  const bool hasLastTs = KOReaderSyncStateStore::loadLastSyncTimestamp(book.path.c_str(), lastTs);
  if (hasLastTs && remote.timestamp > 0 && remote.timestamp <= lastTs) return;  // nothing new on the server

  std::shared_ptr<Epub> epub = std::make_shared<Epub>(book.path, CACHE_DIR);
  int spine = 0, page = 0, pages = 0;
  if (!readSavedProgress(epub->getCachePath(), spine, page, pages) || !epub->load(false, true) ||
      spine >= epub->getSpineItemsCount()) {
    LOG_ERR("SYNCB", "Cannot map remote progress for %s", book.path.c_str());
    book.handled = true;
    failed++;
    return;
  }

  CrossPointPosition local{spine, page, pages};
  local.hasResolvedSpineIndex = true;
  local.hasMappedPage = true;
  const float localPct =
      epub->calculateProgress(spine, pages > 1 ? static_cast<float>(page) / static_cast<float>(pages - 1) : 0.0f);

  // Remote record -> local position, as KOReaderSyncActivity::mapRemote does.
  // The page LUTs belong to the reader's layout, so the reader orientation is
  // applied for the lookup and put back before anything renders.
  CrossPointPosition mapped;
  {
    RenderLock lock(*this);
    const auto uiOrientation = renderer.getOrientation();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    mapped = ProgressMapper::toCrossPoint(epub, KOReaderPosition{remote.progress, remote.percentage}, spine, pages);
    ProgressMapper::refinePageFromLayout(epub, renderer, mapped);
    if (!mapped.hasMappedPage && remote.position.has_value()) {
      if (auto rich = ProgressMapper::fromRichPosition(epub, *remote.position)) {
        ProgressMapper::refinePageFromLayout(epub, renderer, *rich);
        mapped = *rich;
      }
    }
    renderer.setOrientation(uiOrientation);
  }

  const ProgressComparison cmp = compareProgress(local, localPct, mapped, remote.percentage);
  const bool smart = KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
  LOG_DBG("SYNCB", "%s: local %.3f remote %.3f -> %d", book.path.c_str(), localPct, remote.percentage,
          static_cast<int>(cmp));

  if (cmp == ProgressComparison::Synchronized) {
    recordSynced(book, remote.percentage, spine, page, pages, remote.timestamp);
    book.handled = true;
    unchanged++;
    return;
  }
  if (!smart || cmp == ProgressComparison::Unknown) {
    // The choice needs the comparison screen: leave both sides alone.
    book.handled = true;
    book.conflict = true;
    conflicts++;
    return;
  }
  if (cmp == ProgressComparison::LocalAhead) return;  // uploadChanged() sends it

  // Remote ahead: land it in progress.bin. A page count from a different
  // layout is fine; the reader rescales it when the chapter opens.
  const int newSpine = std::max(0, mapped.spineIndex);
  const int newPages = std::max(1, mapped.totalPages);
  const int newPage = std::clamp(mapped.pageNumber, 0, newPages - 1);
  if (!EpubReaderUtils::saveProgress(*epub, newSpine, newPage, newPages)) {
    book.handled = true;
    failed++;
    return;
  }
  recordSynced(book, remote.percentage, newSpine, newPage, newPages, remote.timestamp);
  book.handled = true;
  pulled++;
}

void SyncBooksActivity::recordSynced(const Book& book, const float percentage, const int spine, const int page,
                                     const int pages, const int64_t timestamp) const {
  KOReaderStoredPosition stored;
  stored.percentage = percentage;
  stored.spineIndex = spine;
  stored.pageNumber = page;
  stored.totalPages = pages;
  KOReaderSyncStateStore::saveLastSyncedPosition(book.path.c_str(), stored);
  const int64_t ts = timestamp > 0 ? timestamp : nowIfClockValid();
  if (ts > 0) KOReaderSyncStateStore::saveLastSyncTimestamp(book.path.c_str(), ts);
}

bool SyncBooksActivity::buildUpload(const Book& book, KOReaderProgress& out, int& spine, int& page, int& pages) const {
  std::shared_ptr<Epub> epub = std::make_shared<Epub>(book.path, CACHE_DIR);
  if (!readSavedProgress(epub->getCachePath(), spine, page, pages) || !epub->load(false, true) ||
      spine >= epub->getSpineItemsCount()) {
    return false;
  }
  CrossPointPosition pos{spine, page, pages};
  pos.hasResolvedSpineIndex = true;
  pos.hasMappedPage = true;
  const KOReaderPosition ko = ProgressMapper::toKOReader(epub, pos);
  out = {};
  out.document = book.hash;
  out.progress = ko.xpath;
  out.percentage = ko.percentage;
  out.progressIsGuess = !ko.structural;
  out.position = ProgressMapper::toRichPosition(pos, ko);
  out.metadata = KOReaderMetadata::fromBook(*epub);
  return true;
}

bool SyncBooksActivity::uploadSingle(const size_t index) {
  const Book& book = books[index];
  KOReaderProgress progress;
  int spine = 0, page = 0, pages = 0;
  if (!buildUpload(book, progress, spine, page, pages)) {
    failed++;
    return true;
  }
  KOReaderSyncClient::Error err;
  {
    TlsFramebufferBorrow borrow(renderer);
    err = KOReaderSyncClient::updateProgress(progress);
  }
  if (err == KOReaderSyncClient::OK) {
    recordSynced(book, progress.percentage, spine, page, pages, 0);
    uploaded++;
    return true;
  }
  if (err == KOReaderSyncClient::AUTH_FAILED || err == KOReaderSyncClient::NETWORK_ERROR) {
    failMessage = KOReaderSyncClient::errorString(err);
    return false;
  }
  LOG_ERR("SYNCB", "Upload failed for %s: %s", book.path.c_str(), KOReaderSyncClient::errorString(err));
  failed++;
  return true;
}

bool SyncBooksActivity::flushBatch(KOReaderProgressBatch& batch, std::vector<BatchEntry>& entries) {
  if (entries.empty()) return true;
  KOReaderSyncClient::Error err;
  {
    TlsFramebufferBorrow borrow(renderer);
    err = KOReaderSyncClient::updateProgressBatch(batch);
  }
  if (err == KOReaderSyncClient::OK) {
    for (const auto& e : entries) {
      recordSynced(books[e.index], e.percentage, e.spine, e.page, e.pages, 0);
      uploaded++;
    }
    entries.clear();
    return true;
  }
  if (err == KOReaderSyncClient::BATCH_UNSUPPORTED) {
    LOG_INF("SYNCB", "No batch route; uploading one book per request");
    batchUnsupported = true;
    for (const auto& e : entries) {
      if (!uploadSingle(e.index)) return false;
    }
    entries.clear();
    return true;
  }
  if (err == KOReaderSyncClient::AUTH_FAILED || err == KOReaderSyncClient::NETWORK_ERROR) {
    failMessage = KOReaderSyncClient::errorString(err);
    return false;
  }
  LOG_ERR("SYNCB", "Batch upload failed: %s", KOReaderSyncClient::errorString(err));
  failed += static_cast<unsigned>(entries.size());
  entries.clear();
  return true;
}

bool SyncBooksActivity::uploadChanged() {
  // Books whose position moved here since their last sync (or never synced).
  std::vector<size_t> pending;
  pending.reserve(books.size());
  for (size_t i = 0; i < books.size(); i++) {
    const Book& book = books[i];
    if (book.handled) continue;
    const Epub epub(book.path, CACHE_DIR);
    int spine = 0, page = 0, pages = 0;
    if (!readSavedProgress(epub.getCachePath(), spine, page, pages)) continue;
    KOReaderStoredPosition stored;
    if (KOReaderSyncStateStore::loadLastSyncedPosition(book.path.c_str(), stored) && stored.spineIndex == spine &&
        stored.pageNumber == page) {
      unchanged++;
      continue;
    }
    pending.push_back(i);
  }

  const bool useBatch = KOREADER_STORE.supportsSyncExtensions();
  KOReaderProgressBatch batch;
  std::vector<BatchEntry> entries;
  entries.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH);
  const int count = static_cast<int>(pending.size());
  for (int n = 0; n < count; n++) {
    const size_t i = pending[n];
    if (!useBatch || batchUnsupported) {
      if (!uploadSingle(i)) return false;
    } else {
      // Mapped and serialized now, the Epub freed before any request.
      KOReaderProgress progress;
      BatchEntry entry;
      entry.index = i;
      if (!buildUpload(books[i], progress, entry.spine, entry.page, entry.pages) || !batch.add(progress)) {
        failed++;
      } else {
        entry.percentage = progress.percentage;
        entries.push_back(entry);
      }
      if (batch.count() >= KOReaderSyncClient::MAX_PROGRESS_BATCH && !flushBatch(batch, entries)) return false;
    }
    vTaskDelay(1);
    step(tr(STR_SYNC_UPLOADING_CHANGES), n + 1, count);
  }
  return flushBatch(batch, entries);
}

void SyncBooksActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  // A TLS borrow may have left the framebuffer full of arena data.
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SYNC_BOOKS));

  HalDisplay::RefreshMode refresh = HalDisplay::FULL_REFRESH;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int centerY = pageHeight / 2;

  if (state == State::NoCredentials) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineH, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_KOREADER_SETUP_HINT));
  } else if (state == State::Connecting || state == State::Running) {
    refresh = HalDisplay::FAST_REFRESH;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineH, phase, true, EpdFontFamily::BOLD);
    if (total > 0) {
      const int barW = pageWidth - metrics.contentSidePadding * 4;
      GUI.drawProgressBar(renderer, Rect{(pageWidth - barW) / 2, centerY, barW, 16}, processed, total);
    }
  } else {
    const bool ok = state == State::Done;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
    const char* heading = ok ? (failMessage ? failMessage : tr(STR_SYNC_BOOKS_DONE)) : tr(STR_SYNC_FAILED_MSG);
    renderer.drawCenteredText(UI_10_FONT_ID, y, heading, true, EpdFontFamily::BOLD);
    y += lineH * 2;
    if (!ok && failMessage) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, failMessage);
      y += lineH * 2;
    }
    const struct {
      StrId label;
      unsigned value;
    } rows[] = {{StrId::STR_SYNC_COUNT_UPLOADED, uploaded},
                {StrId::STR_SYNC_COUNT_PULLED, pulled},
                {StrId::STR_SYNC_COUNT_UNCHANGED, unchanged},
                {StrId::STR_SYNC_COUNT_CONFLICTS, conflicts},
                {StrId::STR_SYNC_COUNT_FAILED, failed}};
    const int x = metrics.contentSidePadding * 2;
    for (const auto& row : rows) {
      if (row.value == 0 && (row.label == StrId::STR_SYNC_COUNT_CONFLICTS || row.label == StrId::STR_SYNC_COUNT_FAILED))
        continue;
      char value[12];
      snprintf(value, sizeof(value), "%u", row.value);
      renderer.drawText(UI_10_FONT_ID, x, y, I18N.get(row.label));
      renderer.drawText(UI_10_FONT_ID, pageWidth - x - renderer.getTextWidth(UI_10_FONT_ID, value), y, value, true,
                        EpdFontFamily::BOLD);
      y += lineH;
    }
  }

  if (state != State::Running && state != State::Connecting) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer(refresh);
}
