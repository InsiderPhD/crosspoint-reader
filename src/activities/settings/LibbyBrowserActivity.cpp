#include "LibbyBrowserActivity.h"

#include <AdeptClient.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/BookFusionCoverCache.h"
#include "util/TouchListNav.h"

namespace {
// Books land here, matching what the web UI writes so a library built either
// way looks the same.
constexpr char BOOK_DIR[] = "/Libby";
// The fulfilment token is staged next to the book and removed afterwards; it is
// single-use and worthless once redeemed.
constexpr char ACSM_TMP[] = "/Libby/.loan.acsm";

constexpr int pageIndicatorH = 22;
}  // namespace

void LibbyBrowserActivity::onEnter() {
  Activity::onEnter();

  // Both halves of setup live in the web UI, and each has its own message so
  // the user knows which one they still owe.
  if (LibbyClient::loadIdentity() != LibbyClient::OK) {
    fail(LibbyClient::errorText(LibbyClient::NO_IDENTITY));
    return;
  }
  if (!LibbyClient::hasCredential() || AdeptClient::loadSession() != AdeptClient::OK) {
    fail(AdeptClient::errorText(AdeptClient::NOT_SET_UP));
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    loadLoans();
    return;
  }

  state = WIFI_SELECTION;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void LibbyBrowserActivity::onExit() {
  // Give the TLS contexts' heap back before WiFi is torn down.
  LibbyClient::closeConnection();
  AdeptClient::closeConnection();
  Activity::onExit();
}

void LibbyBrowserActivity::fail(const char* message) {
  {
    RenderLock lock(*this);
    state = ERROR;
    strlcpy(errorMsg, message ? message : "", sizeof(errorMsg));
  }
  requestUpdate();
}

void LibbyBrowserActivity::onWifiSelectionComplete(const bool success) {
  if (!success || WiFi.status() != WL_CONNECTED) {
    finish();
    return;
  }
  loadLoans();
}

void LibbyBrowserActivity::loadLoans() {
  {
    RenderLock lock(*this);
    state = LOADING;
  }
  requestUpdateAndWait();

  LibbyClient::Error err;
  {
    // The loan list arrives over TLS, whose handshake wants a contiguous block
    // this heap often cannot provide. The "Loading" frame is already on the
    // panel and e-ink holds it there, so lend the framebuffer for the call --
    // the same trade BookFusionBrowserActivity makes for its cover fetches.
    TlsFramebufferBorrow borrow(renderer);
    err = LibbyClient::fetchLoans(loans);
  }

  if (err != LibbyClient::OK) {
    fail(LibbyClient::errorText(err));
    return;
  }

  {
    RenderLock lock(*this);
    selectedIndex = 0;
    state = BROWSING;
  }
  requestUpdate();
}

bool LibbyBrowserActivity::onProgress(void* ctx, const char* phase, const size_t done, const size_t total) {
  auto* self = static_cast<LibbyBrowserActivity*>(ctx);
  if (phase) strlcpy(self->sendStatus, phase, sizeof(self->sendStatus));
  self->sendDone = done;
  self->sendTotal = total;
  // Deliberately no repaint: the framebuffer is lent to TLS for the whole
  // exchange, so there is nothing to draw into. The fields are kept current so
  // the frame painted after the borrow is released tells the truth.
  return true;
}

void LibbyBrowserActivity::beginBusyCard(const LibbyLoan& loan, const char* status) {
  {
    RenderLock lock(*this);
    state = SENDING;
    strlcpy(sendTitle, loan.title, sizeof(sendTitle));
    strlcpy(sendAuthor, loan.author, sizeof(sendAuthor));
    strlcpy(sendStatus, status ? status : "", sizeof(sendStatus));
    sendDone = 0;
    sendTotal = 0;
    sendScreenPainted = false;
  }
  // Paint the card and let it reach the panel BEFORE the framebuffer is lent
  // out; it then stays on screen for the whole exchange.
  requestUpdateAndWait();
}

void LibbyBrowserActivity::cacheLoanCover(const LibbyLoan& loan, const char* bookPath) {
  if (!bookPath || !bookPath[0]) return;

  // Epub's cache path is a hash of the file path, so it resolves without
  // opening (or decrypting) the book at all.
  const Epub epub(bookPath, "/.crosspoint");
  epub.setupCacheDir();
  const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
  if (Storage.exists(epub.getThumbBmpPath(coverHeight).c_str())) return;  // already has one

  char coverUrl[192] = {};
  bool fetched = false;
  {
    // Same trade as the loan list and the fulfilment: TLS wants a contiguous
    // block this heap rarely has spare, and the busy card is already on the
    // panel where e-ink holds it.
    TlsFramebufferBorrow borrow(renderer);
    if (LibbyClient::fetchCoverUrl(loan, coverUrl, sizeof(coverUrl))) {
      fetched = BookFusionCoverCache::download(coverUrl, epub);
    }
  }
  if (!fetched) return;

  // The JPEG decoder wants the framebuffer as well, but never at the same time:
  // the fetch's TLS client is destroyed before its borrow ends.
  RenderLock lock(*this);
  JpegScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
  if (!BookFusionCoverCache::convert(epub, coverHeight)) {
    LOG_ERR("LBB", "could not cache the OverDrive cover for %s", bookPath);
  }
  sendScreenPainted = false;  // the lease scrambled the framebuffer
}

void LibbyBrowserActivity::sendSelectedLoan() {
  if (selectedIndex < 0 || selectedIndex >= loans.count) return;
  const LibbyLoan& loan = loans.loans[selectedIndex];

  if (!loan.sendable()) {
    fail(LibbyClient::errorText(LibbyClient::NO_ADOBE_FORMAT));
    return;
  }

  beginBusyCard(loan, tr(STR_LIBBY_ASKING));
  Storage.ensureDirectoryExists(BOOK_DIR);

  LibbyClient::Error libbyErr;
  AdeptClient::Error adeptErr = AdeptClient::OK;
  AdeptClient::Result result;
  {
    TlsFramebufferBorrow borrow(renderer);
    libbyErr = LibbyClient::fetchAcsm(loan, ACSM_TMP);
    if (libbyErr == LibbyClient::OK) {
      adeptErr = AdeptClient::fulfil(ACSM_TMP, BOOK_DIR, result, &LibbyBrowserActivity::onProgress, this);
    }
  }
  // Single-use once redeemed, and it identifies the loan; don't leave it about.
  Storage.remove(ACSM_TMP);

  if (libbyErr != LibbyClient::OK) {
    fail(LibbyClient::errorText(libbyErr));
    return;
  }
  if (adeptErr != AdeptClient::OK) {
    fail(AdeptClient::errorText(adeptErr));
    return;
  }

  // Record where it landed so a later renewal can rewrite this book's licence
  // instead of pulling the whole file down again.
  LibbyClient::rememberBook(loan.id, result.destPath);

  cacheLoanCover(loan, result.destPath);

  {
    RenderLock lock(*this);
    strlcpy(sendTitle, result.title, sizeof(sendTitle));
    strlcpy(completeMsg, tr(STR_LIBBY_SENT), sizeof(completeMsg));
    sendDone = result.bytes;
    state = SEND_COMPLETE;
  }
  requestUpdate();
}

void LibbyBrowserActivity::renewSelectedLoan() {
  if (selectedIndex < 0 || selectedIndex >= loans.count) return;
  const LibbyLoan& loan = loans.loans[selectedIndex];

  beginBusyCard(loan, tr(STR_LIBBY_RENEWING));

  LibbyClient::Error libbyErr;
  AdeptClient::Error adeptErr = AdeptClient::OK;
  char bookPath[128] = {};
  bool haveCopy = false;
  {
    TlsFramebufferBorrow borrow(renderer);
    libbyErr = LibbyClient::renewLoan(loan);
    if (libbyErr == LibbyClient::OK) {
      // Extending the loan on Libby is only half of it: the copy on the card
      // still carries the OLD due date in its .rights sidecar, and the reader
      // enforces that offline. Fetch a fresh token and rewrite just the
      // sidecar -- the encrypted EPUB is unchanged, so it is not re-downloaded.
      haveCopy = LibbyClient::lookupBook(loan.id, bookPath, sizeof(bookPath));
      if (haveCopy) {
        libbyErr = LibbyClient::fetchAcsm(loan, ACSM_TMP);
        if (libbyErr == LibbyClient::OK) {
          adeptErr = AdeptClient::refreshRights(ACSM_TMP, bookPath, &LibbyBrowserActivity::onProgress, this);
        }
      }
    }
  }
  Storage.remove(ACSM_TMP);

  // A renewal is the natural moment to fill in a cover that was never there --
  // and it runs ahead of the error checks on purpose, because the licence
  // refresh failing does not make the artwork any less fetchable.
  if (haveCopy) cacheLoanCover(loan, bookPath);

  if (libbyErr != LibbyClient::OK) {
    fail(LibbyClient::errorText(libbyErr));
    return;
  }
  if (adeptErr != AdeptClient::OK) {
    // Libby already extended the loan by this point; only the licence beside
    // the book is stale. Say both, or this reads as a renewal that never
    // happened and the user renews again.
    char message[sizeof(errorMsg)];
    snprintf(message, sizeof(message), "%s %s", tr(STR_LIBBY_RENEWED_STALE_LICENCE), AdeptClient::errorText(adeptErr));
    fail(message);
    return;
  }

  {
    RenderLock lock(*this);
    // Be explicit when the loan was extended but no copy here could be updated:
    // the user would otherwise assume the book on the card now runs longer.
    strlcpy(completeMsg, haveCopy ? tr(STR_LIBBY_RENEWED) : tr(STR_LIBBY_RENEWED_NO_COPY), sizeof(completeMsg));
    sendDone = 0;
    state = SEND_COMPLETE;
  }
  requestUpdate();
}

void LibbyBrowserActivity::runAction() {
  if (selectedAction == ACTION_RENEW)
    renewSelectedLoan();
  else
    sendSelectedLoan();
}

Rect LibbyBrowserActivity::listRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - pageIndicatorH;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void LibbyBrowserActivity::loop() {
  if (state == WIFI_SELECTION || state == LOADING || state == SENDING) return;

  if (state == ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == SEND_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
    }
    return;
  }

  if (state == LOAN_ACTIONS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state = BROWSING;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      runAction();
      return;
    }
    buttonNavigator.onNext([this] {
      selectedAction = ButtonNavigator::nextIndex(selectedAction, ACTION_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      selectedAction = ButtonNavigator::previousIndex(selectedAction, ACTION_COUNT);
      requestUpdate();
    });
    return;
  }

  // BROWSING
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (loans.count == 0) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowHeight = metrics.listWithSubtitleRowHeight;
  const int pageItems = (rowHeight > 0) ? listRect().height / rowHeight : 1;

  int tappedIndex;
  switch (TouchListNav::tapRow(mappedInput, listRect(), loans.count, selectedIndex,
                               /*hasSubtitle=*/true, tappedIndex)) {
    case TouchListNav::TapResult::SelectionMoved:
      selectedIndex = tappedIndex;
      requestUpdate();
      return;
    case TouchListNav::TapResult::Activated:
      selectedAction = ACTION_SEND;
      {
        RenderLock lock(*this);
        state = LOAN_ACTIONS;
      }
      requestUpdate();
      return;
    case TouchListNav::TapResult::None:
      break;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Send is first and preselected, so the common path stays Confirm-Confirm.
    selectedAction = ACTION_SEND;
    {
      RenderLock lock(*this);
      state = LOAN_ACTIONS;
    }
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, loans.count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, loans.count);
    requestUpdate();
  });
  if (TouchListNav::pageSwipe(mappedInput, loans.count, pageItems, selectedIndex)) {
    requestUpdate();
    return;
  }
  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, loans.count, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, loans.count, pageItems);
    requestUpdate();
  });
}

void LibbyBrowserActivity::drawSendDynamic(const int statusY) {
  const int pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID) + 8;

  // Clear just the band this function owns, so the static card above survives.
  renderer.fillRect(0, statusY - 4, pageWidth, lineH * 2 + 8, false);

  renderer.drawCenteredText(UI_10_FONT_ID, statusY, sendStatus[0] ? sendStatus : tr(STR_LOADING));

  if (sendDone > 0) {
    char line[64];
    if (sendTotal > 0) {
      snprintf(line, sizeof(line), "%.1f / %.1f MB", sendDone / (1024.0f * 1024.0f), sendTotal / (1024.0f * 1024.0f));
    } else {
      snprintf(line, sizeof(line), "%.1f MB", sendDone / (1024.0f * 1024.0f));
    }
    renderer.drawCenteredText(SMALL_FONT_ID, statusY + lineH, line);
  }
}

void LibbyBrowserActivity::render(RenderLock&&) {
  // Fast path: a status change on an already-painted Sending card. The
  // framebuffer persists between renders in single-buffer mode, so repaint only
  // the band that changed.
  if (state == SENDING && sendScreenPainted) {
    drawSendDynamic(sendStatusY);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LIBBY));

  if (state == WIFI_SELECTION || state == LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LOADING));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == ERROR) {
    const int maxWidth = pageWidth - 40;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, errorMsg, maxWidth, 5);
    const int lineH = renderer.getTextHeight(UI_10_FONT_ID) + 8;
    int y = pageHeight / 2 - static_cast<int>(lines.size()) * lineH / 2;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineH;
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SENDING) {
    // Frozen info card. No progress bar: this frame is painted once and then
    // the panel holds it while the framebuffer is lent to TLS for the whole
    // exchange (see sendSelectedLoan).
    constexpr int lineH = 34;
    const int maxWidth = pageWidth - 40;
    char line[128];

    int y = pageHeight / 2 - 2 * lineH;

    renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, sendTitle, maxWidth).c_str(),
                              true, EpdFontFamily::BOLD);
    y += lineH;

    if (sendAuthor[0]) {
      snprintf(line, sizeof(line), "%s", sendAuthor);
      renderer.drawCenteredText(UI_10_FONT_ID, y, renderer.truncatedText(UI_10_FONT_ID, line, maxWidth).c_str());
    }
    y += lineH;

    // Libby does not tell us the file size before fulfilment, so unlike the
    // BookFusion card there is no size line or estimate here. Comics run to
    // well over 100 MB; warn rather than let the screen look stuck.
    renderer.drawCenteredText(SMALL_FONT_ID, y, tr(STR_LIBBY_SEND_WAIT));
    y += lineH;

    sendStatusY = y;
    sendScreenPainted = true;
    drawSendDynamic(y);
    renderer.displayBuffer();
    return;
  }

  if (state == LOAN_ACTIONS) {
    const LibbyLoan& loan = loans.loans[selectedIndex];
    const int maxWidth = pageWidth - 40;
    const auto& m = UITheme::getInstance().getMetrics();
    const int top = m.topPadding + m.headerHeight + m.verticalSpacing;

    renderer.drawCenteredText(UI_10_FONT_ID, top, renderer.truncatedText(UI_10_FONT_ID, loan.title, maxWidth).c_str(),
                              true, EpdFontFamily::BOLD);

    const int listTop = top + renderer.getLineHeight(UI_10_FONT_ID) + m.verticalSpacing * 2;
    const int listH = pageHeight - listTop - m.buttonHintsHeight - m.verticalSpacing;
    GUI.drawList(
        renderer, Rect{0, listTop, pageWidth, listH}, ACTION_COUNT, selectedAction,
        [](int index) -> std::string {
          return std::string(I18N.get(index == ACTION_RENEW ? StrId::STR_LIBBY_RENEW : StrId::STR_LIBBY_SEND));
        },
        nullptr, [](int index) { return index == ACTION_RENEW ? UIIcon::Recent : UIIcon::Book; }, nullptr, true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SEND_COMPLETE) {
    const int maxWidth = pageWidth - 40;
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, completeMsg[0] ? completeMsg : tr(STR_LIBBY_SENT),
                              true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10,
                              renderer.truncatedText(UI_10_FONT_ID, sendTitle, maxWidth).c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // BROWSING
  if (loans.count == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_LIBBY_NO_LOANS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const Rect contentRect = listRect();
  GUI.drawList(
      renderer, contentRect, loans.count, selectedIndex,
      [this](int index) -> std::string { return std::string(loans.loans[index].title); },
      [this](int index) -> std::string { return std::string(loans.loans[index].author); },
      [this](int index) { return loans.loans[index].sendable() ? UIIcon::Book : UIIcon::File; }, nullptr, false);

  // Strike through titles with no Adobe EPUB edition -- audiobooks, magazines,
  // comics in a format this reader can't open. The row stays, so the absence is
  // explained rather than mysterious. Mirrors the non-EPUB overlay in
  // BookFusionBrowserActivity, including its dependence on the theme's
  // title/subtitle offsets.
  {
    const int rowHeight = metrics.listWithSubtitleRowHeight;
    const int pageItems = (rowHeight > 0) ? contentRect.height / rowHeight : 0;
    if (pageItems > 0) {
      const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
      const int titleStrikeY = 7 + renderer.getLineHeight(UI_10_FONT_ID) / 2;
      const int strikeLeft = metrics.contentSidePadding + 8;
      const int strikeRight = pageWidth - metrics.contentSidePadding - 8;
      for (int i = pageStartIndex; i < loans.count && i < pageStartIndex + pageItems; ++i) {
        if (loans.loans[i].sendable()) continue;
        const int itemY = contentRect.y + (i % pageItems) * rowHeight;
        renderer.drawLine(strikeLeft, itemY + titleStrikeY, strikeRight, itemY + titleStrikeY, true);
      }
    }

    // "N / M" page indicator, same footer LibraryActivity uses. All loans are in
    // memory, so the total is exact.
    if (pageItems > 0) {
      const int totalPages = (loans.count + pageItems - 1) / pageItems;
      if (totalPages > 1) {
        char indicator[24];
        snprintf(indicator, sizeof(indicator), "%d / %d", (selectedIndex / pageItems) + 1, totalPages);
        const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
        const int indStripTop = pageHeight - metrics.buttonHintsHeight - pageIndicatorH;
        const int indY = indStripTop + (pageIndicatorH - titleLineHeight) / 2;
        const int indW = renderer.getTextWidth(SMALL_FONT_ID, indicator);
        renderer.drawText(SMALL_FONT_ID, (pageWidth - indW) / 2, indY, indicator, true);
      }
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
