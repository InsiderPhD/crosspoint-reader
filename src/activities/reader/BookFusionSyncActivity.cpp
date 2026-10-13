#include "BookFusionSyncActivity.h"

#include <BluetoothHIDManager.h>
#include <BookFusionBookIdStore.h>
#include <BookFusionTokenStore.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BookFusionBookmarkPush.h"
#include "BookFusionSyncClient.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/TimeUtils.h"

// Implemented in EpubReaderActivity.cpp. We share the same helpers so the
// position math stays in lockstep with the popup-style sync path.
extern bool makeBookFusionPosition(const std::shared_ptr<Epub>& epub, const int spineIndex, const int pageNumber,
                                   const int totalPages, BookFusionPosition& out);
extern bool resolveBookFusionPosition(const std::shared_ptr<Epub>& epub, const BookFusionPosition& pos,
                                      int& outSpineIndex, float& outIntraSpineProgress);
extern BookFusionStoredPosition storedPositionFromBookFusion(const BookFusionPosition& pos);
extern bool formatLocalSyncTimestamp(char* out, size_t outLen);
extern int countBookFusionHighlightsToPush(const std::shared_ptr<Epub>& epub);
extern int countBookFusionHighlightsSynced(const std::shared_ptr<Epub>& epub);
extern int countBookClippings(const std::shared_ptr<Epub>& epub);
extern int hardcoverPushState(const std::shared_ptr<Epub>& epub);
extern const char* hardcoverStatusText(int state, int pushResult);
extern int pushOpenBookToHardcover(const std::shared_ptr<Epub>& epub);
extern int pushBookFusionHighlights(const std::shared_ptr<Epub>& epub, uint32_t bookId,
                                    void (*onProgress)(void* ctx, int current, int total), void* progressCtx);
extern bool resolveBookFusionHighlightOffsets(GfxRenderer& renderer, const std::shared_ptr<Epub>& epub);

namespace {
constexpr int CARD_PADDING = 12;
constexpr int CARD_ROW_GAP = 6;
constexpr int CARD_BAR_HEIGHT = 10;
constexpr int CARD_GAP = 8;
constexpr int BADGE_PADDING_X = 6;

// Tenths of a percent, matching the card's "%.1f%%" label so the "Ahead" badge
// never disagrees with the numbers on screen.
long displayedTenths(const float percentage) { return std::lround(percentage * 10.0f); }

// BookFusion's updated_at ("2026-10-12T21:49:27.000Z", always UTC) as local
// date + time. Empty when it doesn't parse.
std::string formatServerTimestamp(const char* iso) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (iso == nullptr || sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return "";
  }
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return "";
  const uint32_t epoch =
      TimeUtils::getDayOrdinalForDate(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400u +
      static_cast<uint32_t>(hour * 3600 + minute * 60 + second);
  return TimeUtils::formatDateTime(epoch);
}

// Draws `text` in `fontId` so it shares a baseline with a row set in `rowFontId`.
void drawTextOnBaseline(const GfxRenderer& renderer, const int fontId, const int x, const int rowY, const int rowFontId,
                        const char* text) {
  const int y = rowY + renderer.getFontAscenderSize(rowFontId) - renderer.getFontAscenderSize(fontId);
  renderer.drawText(fontId, x, y, text);
}

// smallLines: how many of the counts / last-updated rows the card carries.
int progressCardHeight(const GfxRenderer& renderer, const int smallLines) {
  return CARD_PADDING * 2 + renderer.getLineHeight(UI_10_FONT_ID) + CARD_ROW_GAP +
         renderer.getLineHeight(UI_12_FONT_ID) + CARD_ROW_GAP + CARD_BAR_HEIGHT + CARD_ROW_GAP +
         renderer.getLineHeight(UI_10_FONT_ID) + smallLines * (CARD_ROW_GAP + renderer.getLineHeight(SMALL_FONT_ID));
}

// "Step 1 of 2: <text>" for the one-line status shown while the sync runs.
std::string stepStatus(const int step, const char* text) {
  char label[32];
  snprintf(label, sizeof(label), tr(STR_SYNC_STEP_FORMAT), step, 2);
  return std::string(label) + ": " + text;
}

// "3 bookmarks | 4 highlights"
std::string countsText(const int bookmarks, const int highlights) {
  char buf[40];
  std::string text;
  if (bookmarks == 1) {
    text = tr(STR_BF_BOOKMARK_COUNT_ONE);
  } else {
    snprintf(buf, sizeof(buf), tr(STR_BF_BOOKMARK_COUNT_FORMAT), bookmarks);
    text = buf;
  }
  text += " | ";
  if (highlights == 1) {
    text += tr(STR_BF_HIGHLIGHT_COUNT_ONE);
  } else {
    snprintf(buf, sizeof(buf), tr(STR_BF_HIGHLIGHT_COUNT_FORMAT), highlights);
    text += buf;
  }
  return text;
}
}  // namespace

std::string BookFusionSyncActivity::chapterNameForSpine(int spineIndex) const {
  const int tocIndex = epub ? epub->getTocIndexForSpineIndex(spineIndex) : -1;
  if (tocIndex < 0) return std::string(tr(STR_SECTION_PREFIX)) + std::to_string(spineIndex + 1);
  return epub->getTocItem(tocIndex).title;
}

void BookFusionSyncActivity::fillLocalCard(const float percentage, const int bookmarks, const char* lastSyncIso) {
  localCard.shown = true;
  localCard.percentage = percentage;
  localCard.chapter = chapterNameForSpine(currentSpineIndex);
  localCard.pageText.clear();
  if (totalPages > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), tr(STR_SYNC_PAGE_FORMAT), currentPage + 1, totalPages);
    localCard.pageText = buf;
  }
  localCard.countsLine = countsText(bookmarks, countBookClippings(epub));
  const std::string when = formatServerTimestamp(lastSyncIso);
  if (when.empty()) {
    localCard.updatedLine = tr(STR_BF_NEVER_SYNCED);
  } else {
    char buf[64];
    snprintf(buf, sizeof(buf), tr(STR_SYNC_LAST_SYNCED_FORMAT), when.c_str());
    localCard.updatedLine = buf;
  }
}

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = (direction == Direction::PUSH) ? tr(STR_BF_PREPARING_PUSH) : tr(STR_BF_PREPARING_PULL);
  }
  requestUpdate();
  // BLE and WiFi share the radio; hand it to WiFi cleanly for the sync. The
  // user re-enables Bluetooth afterwards (reader menu / bound button).
  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    LOG_INF("BT", "Disabling Bluetooth for WiFi sync");
    btMgr.disable();
  }
  // The TLS handshake alone wants ~45-50KB of heap (mbedTLS buffers). Drop the
  // cached font glyphs as well; they re-warm on the next page render.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
  performSync();
}

void BookFusionSyncActivity::onExit() {
  Activity::onExit();
  BookFusionSyncClient::closeConnection();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void BookFusionSyncActivity::finishCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void BookFusionSyncActivity::loop() {
  // Input is handled on release, not press: performSync() blocks the main loop
  // inside onEnter(), so the Confirm press that advanced us into the sync (e.g.
  // "Connect" on the WiFi screen) is still held — or already consumed as a
  // press edge — by the time a screen appears here. wasPressed() would wait for
  // a fresh press-down that never comes. Mirrors KOReaderSyncActivity.
  if (state == CHOOSING) {
    using Button = MappedInputManager::Button;
    if (canApplyRemote && (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::Down) ||
                           mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Right))) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (selectedOption == 0 && canApplyRemote) {
        // Apply: hand the resolved position back to the reader; the result
        // handler jumps to the chapter and applies the intra-fraction.
        savePullBaseline();
        setResult(SyncResult{resolvedSpineIndex, 0, resolvedIntra});
        finish();
      } else {
        {
          RenderLock lock(*this);
          state = SYNCING;
        }
        performPush();
      }
    } else if (mappedInput.wasReleased(Button::Back)) {
      finishCancelled();
    }
    return;
  }

  if (state == RESULT_OK || state == RESULT_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (state == RESULT_OK && direction == Direction::PULL) {
        setResult(SyncResult{resolvedSpineIndex, 0, resolvedIntra});
        finish();
      } else {
        // Upload done / any failure: just close, no position change.
        finishCancelled();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Back always cancels — for PULL this means "don't apply the remote
      // position even though we successfully fetched it".
      finishCancelled();
    }
  }
}

void BookFusionSyncActivity::performSync() {
  if (!BF_TOKEN_STORE.hasToken()) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_NO_TOKEN_MSG);
    resultDetail = tr(STR_BF_SETUP_HINT);
    requestUpdate();
    return;
  }
  if (bookId == 0) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_NOT_LINKED_BOOK);
    resultDetail = tr(STR_BF_NOT_A_BF_BOOK);
    requestUpdate();
    return;
  }

  if (direction == Direction::PUSH) {
    performPush();
  } else {
    performPull();  // PULL and ASK both start by fetching BookFusion's position
  }
}

void BookFusionSyncActivity::performPush() {
  {
    RenderLock lock(*this);
    statusMessage = stepStatus(1, tr(STR_BF_READING_POSITION));
  }
  requestUpdateAndWait();

  BookFusionPosition localBfPos;
  if (!makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos)) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_NO_LOCAL_POSITION);
    resultDetail = tr(STR_BF_CHAPTER_NOT_LOADED);
    requestUpdate();
    return;
  }

  const std::string epubPath = epub->getPath();
  BookFusionBookmarkPush bookmarkPush;
  int hardcoverState = 0;  // see hardcoverPushState()
  char previousSyncAt[40] = {};
  {
    // Anchor new clippings for the highlight push before TLS claims the
    // framebuffer. Every render here repaints the whole screen, so the inflate
    // scratch left in the framebuffer needs no cleanup.
    RenderLock lock(*this);
    resolveBookFusionHighlightOffsets(renderer, epub);
    // Bookmarks likewise: the file is read and positions mapped before TLS.
    bookmarkPush.prepare(epub);
    hardcoverState = hardcoverPushState(epub);
    BookFusionBookIdStore::loadLastSyncAt(epubPath.c_str(), previousSyncAt, sizeof(previousSyncAt));
  }

  {
    RenderLock lock(*this);
    // No per-highlight progress here: the TLS block below holds the render lock
    // and lends wolfSSL the framebuffer, so nothing can paint until it ends.
    char msg[64];
    const int pendingHighlights = countBookFusionHighlightsToPush(epub);
    // Kept short: drawCenteredText doesn't wrap, and portrait is only 480px.
    if (pendingHighlights == 1) {
      snprintf(msg, sizeof(msg), tr(STR_BF_UPLOADING_ONE_HIGHLIGHT_FORMAT), localBfPos.percentage);
    } else if (pendingHighlights > 1) {
      snprintf(msg, sizeof(msg), tr(STR_BF_UPLOADING_HIGHLIGHTS_FORMAT), localBfPos.percentage, pendingHighlights);
    } else {
      snprintf(msg, sizeof(msg), tr(STR_BF_UPLOADING_FORMAT), localBfPos.percentage);
    }
    statusMessage = stepStatus(2, msg);
    // setProgress() / trackReadingTime() below are blocking network calls —
    // power the panel down after this paint so the charge pump isn't held
    // powered through a timeout.
    powerDownAfterRender = true;
  }
  requestUpdateAndWait();

  BookFusionPosition uploadedBfPos = localBfPos;
  uint32_t readingTimeSentSeconds = 0;
  int highlightsPushed = 0;
  // BookFusion's own counts; -1 when one couldn't be fetched.
  int remoteBookmarks = -1;
  int remoteHighlights = -1;
  int hardcoverResult = -1;  // pushOpenBookToHardcover()'s return; -1 = not attempted
  auto result = BookFusionSyncClient::NETWORK_ERROR;
  {
    // Lend the framebuffer to wolfSSL for the whole network block (one reused
    // connection: setProgress + trackReadingTime). The guard holds the render
    // lock and closes the connection in its dtor — before the result draw below.
    TlsFramebufferBorrow borrow(renderer);
    result = BookFusionSyncClient::setProgress(bookId, localBfPos, &uploadedBfPos);

    // Reading-time tracking: compute unsent delta and push it to the track endpoint.
    // This is best-effort — a tracking failure doesn't fail the overall push.
    if (result == BookFusionSyncClient::OK) {
      const ReadingBookStats* bookStats = READING_STATS.findBook(epubPath);
      if (bookStats && bookStats->totalReadingMs > 0) {
        const uint64_t currentTotalMs = bookStats->totalReadingMs;
        const uint64_t lastSyncedMs = BookFusionBookIdStore::loadLastSyncedReadingMs(epubPath.c_str());
        if (currentTotalMs > lastSyncedMs) {
          const uint32_t durationSeconds = static_cast<uint32_t>((currentTotalMs - lastSyncedMs) / 1000u);
          if (durationSeconds >= 5) {
            char loggedAt[40] = {};
            if (formatLocalSyncTimestamp(loggedAt, sizeof(loggedAt))) {
              const auto trackResult = BookFusionSyncClient::trackReadingTime(bookId, durationSeconds, loggedAt);
              if (trackResult == BookFusionSyncClient::OK) {
                BookFusionBookIdStore::saveLastSyncedReadingMs(epubPath.c_str(), currentTotalMs);
                readingTimeSentSeconds = durationSeconds;
              } else {
                LOG_DBG("BFS", "Reading time tracking failed (non-fatal): %s",
                        BookFusionSyncClient::errorString(trackResult));
              }
            }
          }
        }
      }
      // Clippings not yet on BookFusion go up as highlights — also best-effort.
      highlightsPushed = pushBookFusionHighlights(epub, bookId, nullptr, nullptr);
      // Bookmarks are separate from the reading position; they only share the connection.
      bookmarkPush.push(bookId);
      BookFusionSyncClient::countBookmarks(bookId, &remoteBookmarks);
      BookFusionSyncClient::countHighlights(bookId, &remoteHighlights);
    }
    // Hardcover, when it is set up, gets the same progress from the stats
    // record. Its own server: close BookFusion's session first so the two never
    // hold TLS buffers at once. Pointless if the network itself is down.
    if (hardcoverState == 3 && result != BookFusionSyncClient::NETWORK_ERROR) {
      BookFusionSyncClient::closeConnection();
      hardcoverResult = pushOpenBookToHardcover(epub);
    }
  }

  RenderLock lock(*this);
  bookmarkPush.commit();
  fillLocalCard(localBfPos.percentage, bookmarkPush.localTotal(), previousSyncAt);
  if (result == BookFusionSyncClient::OK) {
    char syncedAt[40] = {};
    if (uploadedBfPos.updatedAt[0] != '\0') {
      strlcpy(syncedAt, uploadedBfPos.updatedAt, sizeof(syncedAt));
    } else {
      formatLocalSyncTimestamp(syncedAt, sizeof(syncedAt));
    }
    if (syncedAt[0] != '\0') {
      BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), syncedAt, /*pushedByThisDevice=*/true);
    }
    fillLocalCard(localBfPos.percentage, bookmarkPush.localTotal(), syncedAt);
    // BookFusion's side now mirrors what was just uploaded.
    remoteCard.ahead = false;
    localCard.ahead = false;
    baselinePending = false;
    remoteCard.shown = true;
    remoteCard.percentage = localBfPos.percentage;
    remoteCard.chapter = localCard.chapter;
    remoteCard.countsLine =
        countsText(remoteBookmarks >= 0 ? remoteBookmarks : bookmarkPush.syncedTotal(),
                   remoteHighlights >= 0 ? remoteHighlights : countBookFusionHighlightsSynced(epub));
    {
      const std::string when = formatServerTimestamp(syncedAt);
      char buf[96];
      if (when.empty()) {
        strlcpy(buf, tr(STR_SYNC_UPDATED_THIS_DEVICE), sizeof(buf));
      } else {
        snprintf(buf, sizeof(buf), tr(STR_SYNC_UPDATED_THIS_DEVICE_FORMAT), when.c_str());
      }
      remoteCard.updatedLine = buf;
    }
    BookFusionStoredPosition stored = storedPositionFromBookFusion(uploadedBfPos);
    stored.pageNumber = currentPage;
    stored.totalPages = totalPages;
    BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), stored);

    state = RESULT_OK;
    resultTitle = tr(STR_BF_PUSHED);
    if (readingTimeSentSeconds > 0) {
      char timeBuf[48];
      const uint32_t h = readingTimeSentSeconds / 3600u;
      const uint32_t m = (readingTimeSentSeconds % 3600u) / 60u;
      const uint32_t s = readingTimeSentSeconds % 60u;
      if (h > 0) {
        snprintf(timeBuf, sizeof(timeBuf), tr(STR_BF_READING_TIME_HM_FORMAT), h, m);
      } else if (m > 0) {
        snprintf(timeBuf, sizeof(timeBuf), tr(STR_BF_READING_TIME_MS_FORMAT), m, s);
      } else {
        snprintf(timeBuf, sizeof(timeBuf), tr(STR_BF_READING_TIME_S_FORMAT), s);
      }
      resultDetail = timeBuf;
    } else {
      resultDetail.clear();
    }
    LOG_INF("BFS", "Push done: %d new highlights, %d bookmarks on BookFusion", highlightsPushed,
            bookmarkPush.syncedTotal());
  } else {
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_PUSH_FAILED);
    resultDetail = BookFusionSyncClient::errorString(result);
  }
  // Always said when Hardcover is set up, including when there was nothing to send.
  if (const char* hardcoverLine = hardcoverStatusText(hardcoverState, hardcoverResult)) {
    if (!resultDetail.empty()) resultDetail += '\n';
    resultDetail += hardcoverLine;
  }
  requestUpdate();
}

void BookFusionSyncActivity::performPull() {
  {
    RenderLock lock(*this);
    statusMessage = stepStatus(1, tr(STR_FETCH_PROGRESS));
    // getProgress() below is a blocking network call — power the panel down
    // after this paint so the charge pump isn't held powered through a timeout.
    powerDownAfterRender = true;
  }
  requestUpdateAndWait();

  BookFusionPosition remoteBfPos;
  // BookFusion's own counts; -1 when one couldn't be fetched.
  int remoteBookmarks = -1;
  int remoteHighlights = -1;
  auto result = BookFusionSyncClient::NETWORK_ERROR;
  {
    // Lend the framebuffer to wolfSSL for the handshake; the guard closes the
    // connection and frees the render lock before the draws below.
    TlsFramebufferBorrow borrow(renderer);
    result = BookFusionSyncClient::getProgress(bookId, remoteBfPos);
    if (result == BookFusionSyncClient::OK) {
      BookFusionSyncClient::countBookmarks(bookId, &remoteBookmarks);
      BookFusionSyncClient::countHighlights(bookId, &remoteHighlights);
    }
  }

  RenderLock lock(*this);
  if (result == BookFusionSyncClient::NOT_FOUND && direction == Direction::ASK) {
    // Nothing on BookFusion yet: the only thing to offer is an upload.
    BookFusionBookmarkPush bookmarks;
    bookmarks.prepare(epub);
    BookFusionPosition localBfPos;
    const bool haveLocal = makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos);
    fillLocalCard(haveLocal ? localBfPos.percentage : 0.0f, bookmarks.localTotal(), "");
    bookmarks.commit();
    resultTitle = tr(STR_NO_REMOTE_MSG);
    resultDetail.clear();
    canApplyRemote = false;
    selectedOption = 1;
    state = CHOOSING;
    requestUpdate();
    return;
  }
  if (result == BookFusionSyncClient::NOT_FOUND) {
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_NOTHING_TO_PULL);
    resultDetail = tr(STR_BF_NO_REMOTE_PROGRESS);
    requestUpdate();
    return;
  }
  if (result != BookFusionSyncClient::OK) {
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_PULL_FAILED);
    resultDetail = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  // Update status while we resolve the remote position to a local spine/page.
  statusMessage = stepStatus(2, tr(STR_BF_RESOLVING_POSITION));
  requestUpdate();

  int remoteSpineIndex = 0;
  float remoteIntra = 0.0f;
  if (!resolveBookFusionPosition(epub, remoteBfPos, remoteSpineIndex, remoteIntra)) {
    state = RESULT_FAILED;
    resultTitle = tr(STR_BF_PULL_FAILED);
    resultDetail = tr(STR_BF_UNRESOLVED_POSITION);
    requestUpdate();
    return;
  }

  const std::string epubPath = epub->getPath();
  // What this device knew before this pull: tells us who wrote the remote
  // position (the API names no device) and when we last synced.
  char lastSyncAt[40] = {};
  bool lastSyncWasOurPush = false;
  const bool hasLastSyncAt =
      BookFusionBookIdStore::loadLastSyncAt(epubPath.c_str(), lastSyncAt, sizeof(lastSyncAt), &lastSyncWasOurPush);

  // An unchanged remote keeps its "pushed by this device" mark.
  const bool remoteUnchanged =
      hasLastSyncAt && remoteBfPos.updatedAt[0] != '\0' && strcmp(remoteBfPos.updatedAt, lastSyncAt) == 0;
  const bool remoteIsOurs = remoteUnchanged && lastSyncWasOurPush;
  const bool remoteIsOthers = hasLastSyncAt && remoteBfPos.updatedAt[0] != '\0' && !remoteUnchanged;
  fetchedRemote = remoteBfPos;
  fetchedRemoteIsOurs = remoteIsOurs;
  baselinePending = true;
  // PULL records the baseline now, as it always has. ASK waits until the user
  // applies the position: cancelling or uploading must not make the next sync
  // believe this device already took BookFusion's position.
  if (direction == Direction::PULL) savePullBaseline();

  resolvedSpineIndex = remoteSpineIndex;
  resolvedIntra = remoteIntra;
  resolvedPercentage = remoteBfPos.percentage;

  remoteCard.shown = true;
  remoteCard.percentage = remoteBfPos.percentage;
  remoteCard.chapter = chapterNameForSpine(remoteSpineIndex);
  remoteCard.pageText.clear();  // exact page isn't known until pagination
  {
    const std::string when = formatServerTimestamp(remoteBfPos.updatedAt);
    char buf[96] = {};
    if (remoteIsOurs || remoteIsOthers) {
      if (when.empty()) {
        strlcpy(buf, remoteIsOurs ? tr(STR_SYNC_UPDATED_THIS_DEVICE) : tr(STR_SYNC_UPDATED_OTHER_DEVICE), sizeof(buf));
      } else {
        snprintf(buf, sizeof(buf),
                 remoteIsOurs ? tr(STR_SYNC_UPDATED_THIS_DEVICE_FORMAT) : tr(STR_SYNC_UPDATED_OTHER_DEVICE_FORMAT),
                 when.c_str());
      }
    } else if (!when.empty()) {
      // Never synced here, or the last sync was a pull: the writer is unknown.
      snprintf(buf, sizeof(buf), tr(STR_SYNC_UPDATED_FORMAT), when.c_str());
    }
    remoteCard.updatedLine = buf;
  }

  {
    // Counts only: nothing is pushed on a pull.
    BookFusionBookmarkPush bookmarks;
    bookmarks.prepare(epub);
    remoteCard.countsLine =
        countsText(remoteBookmarks >= 0 ? remoteBookmarks : bookmarks.syncedTotal(),
                   remoteHighlights >= 0 ? remoteHighlights : countBookFusionHighlightsSynced(epub));
    BookFusionPosition localBfPos;
    const bool haveLocal = makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos);
    fillLocalCard(haveLocal ? localBfPos.percentage : 0.0f, bookmarks.localTotal(), lastSyncAt);
    bookmarks.commit();
  }
  const long remoteTenths = displayedTenths(remoteCard.percentage);
  const long localTenths = displayedTenths(localCard.percentage);
  remoteCard.ahead = remoteTenths > localTenths;
  localCard.ahead = localTenths > remoteTenths;

  resultDetail.clear();
  if (direction == Direction::ASK) {
    resultTitle = tr(STR_PROGRESS_FOUND);
    canApplyRemote = true;
    // Start on whichever direction moves the reader forward.
    selectedOption = remoteCard.ahead ? 0 : 1;
    state = CHOOSING;
  } else {
    // The button hints say "Cancel / Apply".
    resultTitle = tr(STR_BF_APPLY_REMOTE_PROMPT);
    state = RESULT_OK;
  }
  requestUpdate();
}

// Record the fetched remote position as this book's sync baseline, so the next
// auto-merge sync compares against it rather than the pre-pull position.
void BookFusionSyncActivity::savePullBaseline() {
  if (!baselinePending) return;
  baselinePending = false;
  const std::string epubPath = epub->getPath();
  if (fetchedRemote.updatedAt[0] != '\0') {
    BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), fetchedRemote.updatedAt, fetchedRemoteIsOurs);
  }
  BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), storedPositionFromBookFusion(fetchedRemote));
}

void BookFusionSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  const char* title = tr(STR_SYNC_BOOKFUSION);
  if (direction == Direction::PUSH) title = tr(STR_SYNC_PUSH_BOOKFUSION);
  if (direction == Direction::PULL) title = tr(STR_SYNC_PULL_BOOKFUSION);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  // Hard floor: never paint below this y. Anything that would overflow gets
  // clipped (and we just don't draw it) rather than running off the screen.
  const int yLimit = pageHeight - metrics.buttonHintsHeight - 4;

  if (state == SYNCING) {
    const int centerY = (contentTop + yLimit) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineH, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    int y = contentTop;
    const int margin = metrics.contentSidePadding;
    const int rowSpacing = lineH + 2;  // tight — was lineH + 6
    const int textMaxWidth = pageWidth - margin * 2;

    auto drawLine = [&](const char* text, bool bold = false) {
      if (y + lineH > yLimit) return;  // out of vertical room
      auto truncated = renderer.truncatedText(UI_10_FONT_ID, text, textMaxWidth);
      renderer.drawText(UI_10_FONT_ID, margin, y, truncated.c_str(), true,
                        bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += rowSpacing;
    };

    drawLine(resultTitle.c_str(), /*bold=*/true);
    y += 4;  // small gap below title

    // Card layout:
    //   Title                           [Ahead]
    //   42.5%                         Page 3/12
    //   [=========                           ]
    //   Chapter name...
    //   3 bookmarks | 4 highlights
    //   Last updated by another device, <time>
    auto drawCard = [&](const Rect& card, const char* cardTitle, const ProgressCard& info) {
      renderer.drawRect(card.x, card.y, card.width, card.height, true);
      const int x = card.x + CARD_PADDING;
      const int innerWidth = std::max(1, card.width - CARD_PADDING * 2);
      const int right = x + innerWidth;
      int rowY = card.y + CARD_PADDING;

      const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      int titleRight = right;
      if (info.ahead) {
        const char* badge = tr(STR_SYNC_AHEAD);
        const int badgeWidth = renderer.getTextWidth(SMALL_FONT_ID, badge) + BADGE_PADDING_X * 2;
        const int badgeX = right - badgeWidth;
        renderer.fillRect(badgeX, rowY, badgeWidth, titleLineHeight, true);
        renderer.drawText(SMALL_FONT_ID, badgeX + BADGE_PADDING_X,
                          rowY + (titleLineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2, badge, false);
        titleRight = badgeX - CARD_ROW_GAP * 2;
      }
      const auto titleText =
          renderer.truncatedText(UI_10_FONT_ID, cardTitle, std::max(1, titleRight - x), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, x, rowY, titleText.c_str(), true, EpdFontFamily::BOLD);
      rowY += titleLineHeight + CARD_ROW_GAP;

      char percentStr[16];
      snprintf(percentStr, sizeof(percentStr), "%.1f%%", info.percentage);
      renderer.drawText(UI_12_FONT_ID, x, rowY, percentStr, true, EpdFontFamily::BOLD);
      const int percentRight =
          x + renderer.getTextWidth(UI_12_FONT_ID, percentStr, EpdFontFamily::BOLD) + CARD_ROW_GAP * 2;
      if (!info.pageText.empty() && right - percentRight > 0) {
        const auto page = renderer.truncatedText(UI_10_FONT_ID, info.pageText.c_str(), right - percentRight);
        drawTextOnBaseline(renderer, UI_10_FONT_ID, right - renderer.getTextWidth(UI_10_FONT_ID, page.c_str()), rowY,
                           UI_12_FONT_ID, page.c_str());
      }
      rowY += renderer.getLineHeight(UI_12_FONT_ID) + CARD_ROW_GAP;

      renderer.drawRect(x, rowY, innerWidth, CARD_BAR_HEIGHT, true);
      const float fraction = std::clamp(info.percentage / 100.0f, 0.0f, 1.0f);
      const int fillWidth = static_cast<int>(static_cast<float>(innerWidth - 4) * fraction);
      if (fillWidth > 0) renderer.fillRect(x + 2, rowY + 2, fillWidth, CARD_BAR_HEIGHT - 4, true);
      rowY += CARD_BAR_HEIGHT + CARD_ROW_GAP;

      const auto chapter = renderer.truncatedText(UI_10_FONT_ID, info.chapter.c_str(), innerWidth);
      renderer.drawText(UI_10_FONT_ID, x, rowY, chapter.c_str());
      rowY += renderer.getLineHeight(UI_10_FONT_ID) + CARD_ROW_GAP;

      for (const std::string* line : {&info.countsLine, &info.updatedLine}) {
        if (line->empty()) continue;
        const auto fitted = renderer.truncatedText(SMALL_FONT_ID, line->c_str(), innerWidth);
        renderer.drawText(SMALL_FONT_ID, x, rowY, fitted.c_str());
        rowY += renderer.getLineHeight(SMALL_FONT_ID) + CARD_ROW_GAP;
      }
    };
    const auto smallLines = [](const ProgressCard& info) {
      return (info.countsLine.empty() ? 0 : 1) + (info.updatedLine.empty() ? 0 : 1);
    };

    const bool sideBySide = remoteCard.shown && localCard.shown && pageWidth > pageHeight;
    if (sideBySide) {
      // One shared height so the two cards line up.
      const int cardHeight = progressCardHeight(renderer, std::max(smallLines(remoteCard), smallLines(localCard)));
      if (y + cardHeight <= yLimit) {
        const int cardWidth = (textMaxWidth - CARD_GAP) / 2;
        drawCard(Rect{margin, y, cardWidth, cardHeight}, tr(STR_BF_NAME), remoteCard);
        drawCard(Rect{margin + cardWidth + CARD_GAP, y, cardWidth, cardHeight}, tr(STR_SYNC_THIS_DEVICE), localCard);
        y += cardHeight + CARD_GAP;
      }
    } else {
      const auto drawStacked = [&](const char* cardTitle, const ProgressCard& info) {
        const int cardHeight = progressCardHeight(renderer, smallLines(info));
        if (!info.shown || y + cardHeight > yLimit) return;
        drawCard(Rect{margin, y, textMaxWidth, cardHeight}, cardTitle, info);
        y += cardHeight + CARD_GAP;
      };
      drawStacked(tr(STR_BF_NAME), remoteCard);
      drawStacked(tr(STR_SYNC_THIS_DEVICE), localCard);
    }

    if (state == CHOOSING) {
      // The two directions as selectable rows; Up/Down moves, Confirm runs it.
      const int optionHeight = lineH + 10;
      const auto drawOption = [&](const int index, const char* label) {
        if (y + optionHeight > yLimit) return;
        const bool selected = selectedOption == index;
        if (selected) {
          renderer.fillRect(margin, y, textMaxWidth, optionHeight, true);
        } else {
          renderer.drawRect(margin, y, textMaxWidth, optionHeight, true);
        }
        renderer.drawText(UI_10_FONT_ID, margin + CARD_PADDING, y + (optionHeight - lineH) / 2, label, !selected);
        y += optionHeight + CARD_GAP / 2;
      };
      if (canApplyRemote) drawOption(0, tr(STR_APPLY_REMOTE));
      drawOption(1, tr(STR_UPLOAD_LOCAL));
    }

    // Everything else (reading time, Hardcover, a failure reason) goes in one
    // more card, a line each.
    int detailLines = 0;
    if (!resultDetail.empty()) {
      detailLines = 1 + static_cast<int>(std::count(resultDetail.begin(), resultDetail.end(), '\n'));
    }
    const int detailCardHeight = CARD_PADDING * 2 + detailLines * lineH + (detailLines - 1) * CARD_ROW_GAP;
    if (detailLines > 0 && y + detailCardHeight <= yLimit) {
      renderer.drawRect(margin, y, textMaxWidth, detailCardHeight, true);
      const int x = margin + CARD_PADDING;
      const int innerWidth = std::max(1, textMaxWidth - CARD_PADDING * 2);
      int rowY = y + CARD_PADDING;
      const auto drawDetail = [&](const char* text) {
        // Falls back to the small font before truncating.
        const int font = renderer.getTextWidth(UI_10_FONT_ID, text) <= innerWidth ? UI_10_FONT_ID : SMALL_FONT_ID;
        const auto fitted = renderer.truncatedText(font, text, innerWidth);
        drawTextOnBaseline(renderer, font, x, rowY, UI_10_FONT_ID, fitted.c_str());
        rowY += lineH + CARD_ROW_GAP;
      };
      size_t start = 0;
      while (start < resultDetail.size()) {
        const size_t nl = resultDetail.find('\n', start);
        const size_t end = (nl == std::string::npos) ? resultDetail.size() : nl;
        drawDetail(resultDetail.substr(start, end - start).c_str());
        start = end + 1;
      }
      y += detailCardHeight + CARD_GAP;
    }

    const bool isPullPrompt = (state == RESULT_OK && direction == Direction::PULL);
    const char* confirmLabel = isPullPrompt ? tr(STR_SYNC_APPLY) : tr(STR_CONFIRM);
    const char* backLabel = isPullPrompt ? tr(STR_CANCEL) : tr(STR_BACK);
    const auto labels = (state == CHOOSING)
                            ? mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                            : mappedInput.mapLabels(backLabel, confirmLabel, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();

  // When this paint precedes a blocking network call, power the EPD rails down
  // afterwards so the charge pump isn't held at high voltage drawing current
  // alongside WiFi TX spikes during the (potentially many-second) wait — that
  // combination was causing severe ghosting on sync timeouts. The next paint
  // wakes from off and the SDK auto-promotes it to a HALF refresh, re-powering
  // the panel and scrubbing any residue in one go. See EInkDisplay.cpp
  // displayBuffer() (wake-from-off HALF promotion) and refreshDisplay()
  // (ANALOG_OFF/CLOCK_OFF on turnOffScreen).
  const bool powerOff = powerDownAfterRender;
  powerDownAfterRender = false;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, powerOff);
}
