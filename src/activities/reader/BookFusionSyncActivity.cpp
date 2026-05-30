#include "BookFusionSyncActivity.h"

#include <BookFusionBookIdStore.h>
#include <BookFusionTokenStore.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "BookFusionSyncClient.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Implemented in EpubReaderActivity.cpp. We share the same helpers so the
// position math stays in lockstep with the popup-style sync path.
extern bool makeBookFusionPosition(const std::shared_ptr<Epub>& epub, const int spineIndex, const int pageNumber,
                                   const int totalPages, BookFusionPosition& out);
extern bool resolveBookFusionPosition(const std::shared_ptr<Epub>& epub, const BookFusionPosition& pos,
                                      int& outSpineIndex, float& outIntraSpineProgress);
extern BookFusionStoredPosition storedPositionFromBookFusion(const BookFusionPosition& pos);
extern bool formatLocalSyncTimestamp(char* out, size_t outLen);

std::string BookFusionSyncActivity::chapterNameForSpine(int spineIndex) const {
  if (!epub) return std::string("Chapter ") + std::to_string(spineIndex + 1);
  const int tocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (tocIndex < 0) return std::string("Chapter ") + std::to_string(spineIndex + 1);
  return epub->getTocItem(tocIndex).title;
}

void BookFusionSyncActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = (direction == Direction::PUSH) ? "Preparing to push…" : "Preparing to pull…";
  }
  requestUpdate();
  performSync();
}

void BookFusionSyncActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void BookFusionSyncActivity::loop() {
  if (state == RESULT_OK || state == RESULT_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (state == RESULT_OK && direction == Direction::PULL) {
        // Apply: hand the resolved position back to the reader; the result
        // handler jumps to the chapter and applies the intra-fraction.
        setResult(SyncResult{resolvedSpineIndex, 0, resolvedIntra});
      } else {
        // PUSH success / any failure: just close, no position change.
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
      }
      finish();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      // Back always cancels — for PULL this means "don't apply the remote
      // position even though we successfully fetched it".
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
  }
}

void BookFusionSyncActivity::performSync() {
  if (!BF_TOKEN_STORE.hasToken()) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = "No BookFusion account";
    resultDetail = "Link a BookFusion account in Settings first.";
    requestUpdate();
    return;
  }
  if (bookId == 0) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = "Not a BookFusion book";
    resultDetail = "This book isn't linked to a BookFusion entry.";
    requestUpdate();
    return;
  }

  if (direction == Direction::PUSH) {
    performPush();
  } else {
    performPull();
  }
}

void BookFusionSyncActivity::performPush() {
  {
    RenderLock lock(*this);
    statusMessage = "Step 1 of 2: Reading local position…";
  }
  requestUpdateAndWait();

  BookFusionPosition localBfPos;
  if (!makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos)) {
    RenderLock lock(*this);
    state = RESULT_FAILED;
    resultTitle = "Could not build local position";
    resultDetail = "The reader hasn't fully loaded this chapter yet. Try again in a moment.";
    requestUpdate();
    return;
  }

  // Capture local details for the result screen so the user can see exactly
  // what got uploaded. Kept terse — chapter name + percentage is enough.
  localChapter = chapterNameForSpine(currentSpineIndex);
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f%% of book", localBfPos.percentage);
    localPercentLine = buf;
  }
  localPageLine.clear();
  hasLocalDetails = true;

  {
    RenderLock lock(*this);
    char msg[64];
    snprintf(msg, sizeof(msg), "Step 2 of 2: Uploading %.1f%%…", localBfPos.percentage);
    statusMessage = msg;
  }
  requestUpdateAndWait();

  BookFusionPosition uploadedBfPos = localBfPos;
  const auto result = BookFusionSyncClient::setProgress(bookId, localBfPos, &uploadedBfPos);

  RenderLock lock(*this);
  if (result == BookFusionSyncClient::OK) {
    const std::string epubPath = epub->getPath();
    if (uploadedBfPos.updatedAt[0] != '\0') {
      BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), uploadedBfPos.updatedAt);
    } else {
      char localTs[40];
      if (formatLocalSyncTimestamp(localTs, sizeof(localTs))) {
        BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), localTs);
      }
    }
    BookFusionStoredPosition stored = storedPositionFromBookFusion(uploadedBfPos);
    stored.pageNumber = currentPage;
    stored.totalPages = totalPages;
    BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), stored);

    state = RESULT_OK;
    resultTitle = "Pushed to BookFusion";
    resultDetail.clear();
  } else {
    state = RESULT_FAILED;
    resultTitle = "Push failed";
    resultDetail = BookFusionSyncClient::errorString(result);
  }
  requestUpdate();
}

void BookFusionSyncActivity::performPull() {
  {
    RenderLock lock(*this);
    statusMessage = "Step 1 of 2: Fetching remote progress…";
  }
  requestUpdateAndWait();

  BookFusionPosition remoteBfPos;
  const auto result = BookFusionSyncClient::getProgress(bookId, remoteBfPos);

  RenderLock lock(*this);
  if (result == BookFusionSyncClient::NOT_FOUND) {
    state = RESULT_FAILED;
    resultTitle = "Nothing to pull";
    resultDetail = "BookFusion has no saved progress for this book yet.";
    requestUpdate();
    return;
  }
  if (result != BookFusionSyncClient::OK) {
    state = RESULT_FAILED;
    resultTitle = "Pull failed";
    resultDetail = BookFusionSyncClient::errorString(result);
    requestUpdate();
    return;
  }

  // Update status while we resolve the remote position to a local spine/page.
  statusMessage = "Step 2 of 2: Resolving position…";
  requestUpdate();

  int remoteSpineIndex = 0;
  float remoteIntra = 0.0f;
  if (!resolveBookFusionPosition(epub, remoteBfPos, remoteSpineIndex, remoteIntra)) {
    state = RESULT_FAILED;
    resultTitle = "Pull failed";
    resultDetail = "Couldn't resolve the remote position to a chapter in this EPUB.";
    requestUpdate();
    return;
  }

  // Persist the new baseline so the next auto-merge sync compares against the
  // applied remote, not the position we had before pulling.
  const std::string epubPath = epub->getPath();
  if (remoteBfPos.updatedAt[0] != '\0') {
    BookFusionBookIdStore::saveLastSyncAt(epubPath.c_str(), remoteBfPos.updatedAt);
  }
  BookFusionBookIdStore::saveLastSyncedPosition(epubPath.c_str(), storedPositionFromBookFusion(remoteBfPos));

  resolvedSpineIndex = remoteSpineIndex;
  resolvedIntra = remoteIntra;
  resolvedPercentage = remoteBfPos.percentage;

  // Populate both sides of the comparison. Keep it terse — landscape only has
  // ~340 px of content height after the header + button hints, so every line
  // counts.
  remoteChapter = chapterNameForSpine(remoteSpineIndex);
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f%% of book", remoteBfPos.percentage);
    remotePercentLine = buf;
  }
  remotePageLine.clear();  // omit — exact page isn't known until pagination
  hasRemoteDetails = true;

  localChapter = chapterNameForSpine(currentSpineIndex);
  {
    BookFusionPosition localBfPos;
    if (makeBookFusionPosition(epub, currentSpineIndex, currentPage, totalPages, localBfPos)) {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.1f%% of book", localBfPos.percentage);
      localPercentLine = buf;
    } else {
      localPercentLine.clear();
    }
  }
  localPageLine.clear();
  hasLocalDetails = true;

  state = RESULT_OK;
  resultTitle = "Apply remote progress?";
  // No body detail — the button hints already say "Cancel / Apply"; an extra
  // paragraph was pushing content off the bottom in landscape.
  resultDetail.clear();
  requestUpdate();
}

void BookFusionSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  const char* title = (direction == Direction::PUSH) ? "BookFusion: Push Local Progress"
                                                     : "BookFusion: Pull Remote Progress";
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

    if (hasRemoteDetails) {
      drawLine("Remote (BookFusion):", /*bold=*/true);
      if (!remoteChapter.empty()) drawLine((std::string("  ") + remoteChapter).c_str());
      if (!remotePercentLine.empty()) drawLine((std::string("  ") + remotePercentLine).c_str());
      y += 4;
    }
    if (hasLocalDetails) {
      drawLine("Local (this device):", /*bold=*/true);
      if (!localChapter.empty()) drawLine((std::string("  ") + localChapter).c_str());
      if (!localPercentLine.empty()) drawLine((std::string("  ") + localPercentLine).c_str());
    }

    if (!resultDetail.empty()) {
      std::string remaining = resultDetail;
      while (!remaining.empty()) {
        auto nl = remaining.find('\n');
        std::string chunk = (nl == std::string::npos) ? remaining : remaining.substr(0, nl);
        drawLine(chunk.c_str());
        if (nl == std::string::npos) break;
        remaining.erase(0, nl + 1);
      }
    }

    const bool isPullPrompt = (state == RESULT_OK && direction == Direction::PULL);
    const char* confirmLabel = isPullPrompt ? "Apply" : tr(STR_CONFIRM);
    const char* backLabel = isPullPrompt ? "Cancel" : tr(STR_BACK);
    const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (SETTINGS.darkMode) renderer.invertScreen();
  renderer.displayBuffer();
}
