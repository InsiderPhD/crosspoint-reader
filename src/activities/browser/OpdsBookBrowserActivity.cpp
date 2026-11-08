#include "OpdsBookBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsFeedParser.h>
#include <OpdsJsonParser.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/home/LibraryActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloadFilename.h"
#include "network/HttpDownloader.h"
#include "util/OpdsDownloadFolder.h"
#include "util/OpdsFeedError.h"
#include "util/StringUtils.h"
#include "util/TouchListNav.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

// Tells a dead server from a catalog that wants a password; both are a bare
// "failed" to the fetch itself.
StrId feedFailureMessage(const HttpDownloader::FetchResult& fetch) {
  switch (classifyFeedFailure(fetch.status, fetch.offOrigin)) {
    case OpdsFeedError::Unreachable:
      return StrId::STR_FEED_UNREACHABLE;
    case OpdsFeedError::Unauthorized:
      return StrId::STR_FEED_UNAUTHORIZED;
    case OpdsFeedError::Redirected:
      return StrId::STR_FEED_REDIRECTED;
    case OpdsFeedError::ServerError:
      return StrId::STR_FEED_SERVER_ERROR;
    case OpdsFeedError::Other:
      break;
  }
  return StrId::STR_FETCH_FEED_FAILED;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  serverIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();

  const size_t count = OPDS_STORE.getCount();
  if (count == 0) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
  } else if (count == 1) {
    openServer(0);
  } else {
    showServerSelect();
  }
}

void OpdsBookBrowserActivity::showServerSelect() {
  state = BrowserState::SERVER_SELECT;
  entries.clear();
  navigationHistory.clear();
  searchTemplate.clear();
  currentPath.clear();
  selectorIndex = 0;
  requestUpdate();
}

void OpdsBookBrowserActivity::openServer(const size_t index) {
  const OpdsServer* picked = OPDS_STORE.getServer(index);
  if (!picked) return;
  server = *picked;
  auth = HttpDownloader::ScopedAuth{UrlUtils::origin(UrlUtils::ensureProtocol(server.url)), server.username,
                                    server.password, server.activeCustomHeaders()};
  state = BrowserState::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();
  checkAndConnectWifi();
}

// Shared by render() and loop()'s tap hit-testing so the two can never disagree.
Rect OpdsBookBrowserActivity::serverListRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  return Rect{0, contentTop, renderer.getScreenWidth(), contentHeight};
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

// Top edge of the first row's highlight band. Mirrors render()'s
// fillRect(0, 60 + row * 30 - 2, ..., 30): the band starts 2px above the text
// baseline row. This screen draws no orientation gutters, so there is no
// content offset to account for. Keep in sync with render().
int OpdsBookBrowserActivity::listTopY() const { return 60 - 2; }

// The BROWSING Confirm short-press body, also fired by a Full Touch tap on the
// selected row.
void OpdsBookBrowserActivity::activateSelectedEntry() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;

  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? showDescription(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::showDescription(const OpdsEntry& book) {
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  descriptionText.clear();
  previewDownloadUrl.clear();
  TlsFramebufferBorrow borrow(renderer);
  if (book.indirect) {
    // OPDS 2 indirect acquisition: the row points at a publication document,
    // which carries the description and the EPUB link (possibly via further
    // documents; at most MAX_INDIRECT_HOPS). One-entry parser per hop.
    constexpr int MAX_INDIRECT_HOPS = 3;
    std::string url = UrlUtils::resolveReference(feedUrl, book.href);
    for (int hop = 0; hop < MAX_INDIRECT_HOPS; ++hop) {
      OpdsJsonParser parser(/*publicationDocument=*/true);
      parser.setPreferredLanguage(I18n::languageCode(I18N.getLanguage()));
      parser.captureDescriptionFor("", DESCRIPTION_MAX_BYTES);
      OpdsParserStream stream{parser};
      if (!HttpDownloader::fetchUrl(url, stream, auth, nullptr)) break;
      parser.flush();
      if (parser.error() || parser.getEntries().empty()) break;
      std::string text = parser.takeDescription();
      if (!text.empty()) descriptionText = std::move(text);
      const OpdsEntry& detail = parser.getEntries().front();
      if (detail.href.empty()) break;
      const std::string next = UrlUtils::resolveReference(url, detail.href);
      if (!detail.indirect) {
        previewDownloadUrl = next;
        break;
      }
      url = next;
    }
    if (previewDownloadUrl.empty()) LOG_ERR("OPDS", "No EPUB behind the publication document");
  } else {
    // A second, description-only parse of the same feed: it stores no entries,
    // so the transient cost is the parser plus one bounded excerpt buffer, not
    // a copy of the catalog. A failed fetch still opens the preview (without a
    // description) so the book stays downloadable.
    previewDownloadUrl = UrlUtils::resolveReference(feedUrl, book.href);
    OpdsFeedParser parser;
    parser.setPreferredLanguage(I18n::languageCode(I18N.getLanguage()));
    parser.captureDescriptionFor(book.href, DESCRIPTION_MAX_BYTES);
    {
      OpdsParserStream stream{parser};
      if (!HttpDownloader::fetchUrl(feedUrl, stream, auth, nullptr)) LOG_ERR("OPDS", "Could not load the description");
    }
    if (parser) descriptionText = parser.takeDescription();
  }
  descriptionTop = 0;
  state = BrowserState::DESCRIPTION;
  requestUpdate();
}

void OpdsBookBrowserActivity::closeDescription() {
  std::string().swap(descriptionText);
  std::string().swap(previewDownloadUrl);
  descriptionTop = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::SERVER_SELECT) {
    const int count = static_cast<int>(OPDS_STORE.getCount());
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    int tappedIndex;
    switch (
        TouchListNav::tapRow(mappedInput, serverListRect(), count, serverIndex, /*hasSubtitle=*/true, tappedIndex)) {
      case TouchListNav::TapResult::SelectionMoved:
        serverIndex = tappedIndex;
        requestUpdate();
        return;
      case TouchListNav::TapResult::Activated:
        serverIndex = tappedIndex;
        openServer(static_cast<size_t>(serverIndex));
        return;
      case TouchListNav::TapResult::None:
        break;
    }
    // On the press, not the release: the press that opened this screen from
    // File Transfer releases here. The release that follows this press belongs
    // to the picker, so it must not reach the catalog list.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      consumeConfirm = true;
      openServer(static_cast<size_t>(serverIndex));
      return;
    }
    buttonNavigator.onNextRelease([this, count] {
      serverIndex = ButtonNavigator::nextIndex(serverIndex, count);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this, count] {
      serverIndex = ButtonNavigator::previousIndex(serverIndex, count);
      requestUpdate();
    });
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? finish() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::DESCRIPTION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size())) {
        std::string().swap(descriptionText);
        if (previewDownloadUrl.empty()) {
          // An indirect publication whose documents led to no plain EPUB.
          state = BrowserState::ERROR;
          errorMessage = tr(STR_OPDS_NO_EPUB);
          requestUpdate();
          return;
        }
        const std::string url = std::move(previewDownloadUrl);
        previewDownloadUrl.clear();
        downloadBook(entries[selectorIndex], url);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeDescription();
      return;
    }
    // A page of lines per press; render() clamps the end.
    const auto scroll = [this](const int direction) {
      const int next = std::max(0, descriptionTop + direction * std::max(1, descriptionPageLines - 1));
      if (next != descriptionTop) {
        descriptionTop = next;
        requestUpdate();
      }
    };
    buttonNavigator.onNextRelease([&scroll] { scroll(1); });
    buttonNavigator.onPreviousRelease([&scroll] { scroll(-1); });
    return;
  }

  if (state == BrowserState::BROWSING) {
#if FREEINK_CAP_TOUCH
    // Full Touch: first tap on a row moves the cursor there; a second tap on the
    // already-selected row opens (or downloads) it. Rows are page-relative, so
    // the tapped entry is the current page start plus the row.
    if (CrossPointSettings::FULL_TOUCH_UI && !entries.empty()) {
      int lx, ly;
      if (mappedInput.wasTapPoint(lx, ly)) {
        const int top = listTopY();
        const int row = (ly - top) / ROW_H;
        if (ly >= top && row >= 0 && row < PAGE_ITEMS) {
          const int itemIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS + row;
          if (itemIndex < static_cast<int>(entries.size())) {
            const bool activate = TouchListNav::tapActivates(itemIndex, selectorIndex);
            selectorIndex = itemIndex;
            if (activate) {
              activateSelectedEntry();
            } else {
              requestUpdate();
            }
            return;
          }
        }
        // Dead space (title bar, below the last row): no action.
      }
    }
#endif

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelectedEntry();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    if (!entries.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      // Full Touch: a vertical swipe turns a page, matching the held side key.
      if (TouchListNav::pageSwipe(mappedInput, static_cast<int>(entries.size()), PAGE_ITEMS, selectorIndex)) {
        requestUpdate();
        return;
      }
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::SERVER_SELECT:
      return false;
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::DESCRIPTION:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == BrowserState::SERVER_SELECT) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_OPDS_SERVERS));
    GUI.drawList(
        renderer, serverListRect(), static_cast<int>(OPDS_STORE.getCount()), serverIndex,
        [](int index) {
          const OpdsServer* s = OPDS_STORE.getServer(static_cast<size_t>(index));
          return s ? s->label() : std::string();
        },
        [](int index) {
          const OpdsServer* s = OPDS_STORE.getServer(static_cast<size_t>(index));
          return s && !s->name.empty() ? s->url : std::string();
        });
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_OPDS_BROWSER), true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DESCRIPTION && selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size())) {
    const auto& book = entries[selectorIndex];
    constexpr int SIDE = 20;
    const int textWidth = pageWidth - 2 * SIDE;
    int y = 50;
    for (const auto& line :
         renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 2, EpdFontFamily::BOLD)) {
      renderer.drawText(UI_12_FONT_ID, SIDE, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_12_FONT_ID);
    }
    if (!book.author.empty()) {
      const auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, SIDE, y, author.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += 10;
    renderer.drawLine(SIDE, y, pageWidth - SIDE, y);
    y += 10;

    const auto& metrics = UITheme::getInstance().getMetrics();
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int bottom = pageHeight - metrics.buttonHintsHeight - 30;
    descriptionPageLines = std::max(1, (bottom - y) / lineHeight);
    if (descriptionText.empty()) {
      renderer.drawText(UI_10_FONT_ID, SIDE, y, tr(STR_NO_DESCRIPTION));
    } else {
      // Wrapped per paint from the bounded excerpt: no line list is kept
      // between frames.
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, descriptionText.c_str(), textWidth, INT16_MAX);
      const int lastTop = std::max(0, static_cast<int>(lines.size()) - descriptionPageLines);
      if (descriptionTop > lastTop) descriptionTop = lastTop;
      for (int i = 0; i < descriptionPageLines && descriptionTop + i < static_cast<int>(lines.size()); ++i) {
        renderer.drawText(UI_10_FONT_ID, SIDE, y + i * lineHeight, lines[descriptionTop + i].c_str());
      }
      if (lines.size() > static_cast<size_t>(descriptionPageLines)) {
        const int step = std::max(1, descriptionPageLines - 1);
        char page[24];
        snprintf(page, sizeof(page), "%d / %d", (descriptionTop + step - 1) / step + 1,
                 (lastTop + step - 1) / step + 1);
        renderer.drawCenteredText(UI_10_FONT_ID, bottom + 4, page);
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    // Static frame: the screen freezes on this while the framebuffer is lent
    // to the transfer's TLS session — no live progress (serial has a per-MB
    // heartbeat instead).
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING), true, EpdFontFamily::BOLD);
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 20, tr(STR_DOWNLOAD_WAIT));
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_PREVIEW) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    // Row bands start at listTopY() (shared with the tap hit-testing in loop()).
    const int listTop = listTopY();
    renderer.fillRect(0, listTop + (selectorIndex % PAGE_ITEMS) * ROW_H, pageWidth - 1, ROW_H);

    for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& entry = entries[i];
      const int rowY = listTop + 2 + (i % PAGE_ITEMS) * ROW_H;
      const bool black = i != static_cast<size_t>(selectorIndex);
      // A category's advertised size, right-aligned; the title gives up the room.
      int titleWidth = pageWidth - 40;
      if (entry.type == OpdsEntryType::NAVIGATION && entry.count >= 0) {
        char countLabel[16];
        snprintf(countLabel, sizeof(countLabel), "(%ld)", static_cast<long>(entry.count));
        const int countWidth = renderer.getTextWidth(UI_10_FONT_ID, countLabel);
        renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - countWidth, rowY, countLabel, black);
        titleWidth -= countWidth + 10;
      }
      std::string displayText = (entry.type == OpdsEntryType::NAVIGATION) ? "> " + entry.title : entry.title;
      if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) displayText += " - " + entry.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), titleWidth);
      renderer.drawText(UI_10_FONT_ID, 20, rowY, item.c_str(), black);
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  OpdsFeedParser parser;  // Atom or OPDS 2 JSON, chosen by the first byte
  parser.setPreferredLanguage(I18n::languageCode(I18N.getLanguage()));
  {
    // Lend the framebuffer to the catalog fetch's TLS session ("Loading…" is
    // already queued/painted; nothing renders during the call).
    TlsFramebufferBorrow borrow(renderer);
    OpdsParserStream stream{parser};
    HttpDownloader::FetchResult fetch;
    if (!HttpDownloader::fetchUrl(url, stream, auth, &fetch)) {
      state = BrowserState::ERROR;
      // A parser failure rejects the write and aborts the download; report it as a parse
      // error rather than hiding it behind a generic fetch failure.
      errorMessage = parser.error() ? tr(STR_PARSE_FEED_FAILED) : I18N.get(feedFailureMessage(fetch));
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  // Resolved now, against the feed that declared it: performSearch() fetches
  // the filled-in template as is.
  searchTemplate =
      parser.getSearchTemplate().empty() ? std::string() : UrlUtils::resolveReference(url, parser.getSearchTemplate());
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = parser.takeEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::resolveReference(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    // Back out of a catalog's root: to the picker when there is a choice,
    // otherwise to File Transfer.
    if (OPDS_STORE.getCount() > 1) {
      showServerSelect();
      return;
    }
    finish();  // back to File Transfer
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

namespace {
// Renames a finished download to the name the server sent, when that name is
// usable. Returns the path the book ends up at. Anything doubtful keeps the
// generated "Author - Title.epub": no filename on the header, a name FAT or
// this firmware cannot take (FsHelpers::isValidFileName), a hidden file, a
// non-EPUB extension, or a name that already exists (never overwrite a
// different book because a server reused a name).
std::string applyServerFilename(const std::string& downloadedPath, const std::string& contentDisposition) {
  std::string serverName;
  if (!HttpDownloadFilename::parse(contentDisposition, serverName)) return downloadedPath;
  if (!FsHelpers::isValidFileName(serverName) || serverName.front() == '.' || serverName.front() == ' ' ||
      !FsHelpers::hasEpubExtension(serverName)) {
    LOG_INF("OPDS", "Server file name rejected, keeping %s", downloadedPath.c_str());
    return downloadedPath;
  }
  // Same 100-byte budget as the generated names; the stem is cut on a UTF-8
  // codepoint boundary and the extension kept.
  // Same folder as the generated name.
  const size_t slash = downloadedPath.rfind('/');
  std::string target =
      downloadedPath.substr(0, slash + 1) + StringUtils::sanitizeFilenamePreservingExtension(serverName);
  if (target == downloadedPath || !FsHelpers::hasEpubExtension(target)) return downloadedPath;
  if (Storage.exists(target.c_str())) {
    LOG_INF("OPDS", "%s already exists, keeping %s", target.c_str(), downloadedPath.c_str());
    return downloadedPath;
  }
  if (!Storage.rename(downloadedPath.c_str(), target.c_str())) {
    LOG_ERR("OPDS", "Rename to server file name failed, keeping %s", downloadedPath.c_str());
    return downloadedPath;
  }
  return target;
}
}  // namespace

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book, const std::string& downloadUrl) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  // Paint the static download frame and wait for it: the screen stays frozen
  // on it while the framebuffer is lent to the transfer's TLS session below
  // (same scheme as BookFusion book downloads and OTA).
  requestUpdateAndWait();

  // The catalog's own folder, else the shared default; both re-validated here
  // (the shared one may have been typed on the web page).
  std::string folder;
  if (!OpdsDownloadFolder::normalize(OpdsDownloadFolder::effective(server.downloadFolder, SETTINGS.opdsDownloadFolder),
                                     folder)) {
    LOG_ERR("OPDS", "Invalid download folder");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_OPDS_INVALID_FOLDER);
    requestUpdate();
    return;
  }
  // normalize() gives "" (SD root) or "/A/B" without a trailing slash.
  if (folder.empty() || folder == "/") {
    folder = "/";
  } else {
    if (!Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
      LOG_ERR("OPDS", "Could not create download folder %s", folder.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    folder += '/';
  }
  std::string filename =
      folder + StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) + ".epub";
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  auto result = HttpDownloader::HTTP_ERROR;
  // Only filled when the option is on; one short-lived string per download.
  std::string contentDisposition;
  {
    TlsFramebufferBorrow borrow(renderer);
    size_t lastLoggedMB = 0;
    result = HttpDownloader::downloadToFile(
        downloadUrl, filename,
        [&lastLoggedMB](const size_t downloaded, const size_t total) {
          const size_t mb = downloaded >> 20;
          if (mb > lastLoggedMB) {
            lastLoggedMB = mb;
            if (total > 0) {
              LOG_DBG("OPDS", "Download progress: %u/%u MB", (unsigned)mb, (unsigned)(total >> 20));
            } else {
              LOG_DBG("OPDS", "Download progress: %u MB", (unsigned)mb);
            }
          }
        },
        /*legacyConfiguredAuth=*/false, /*expectedSize=*/0, /*cancelFlag=*/nullptr, /*username=*/"",
        /*password=*/"", /*headers=*/{}, /*resumePartial=*/false,
        SETTINGS.opdsUseServerFilename ? &contentDisposition : nullptr, &auth);
  }

  if (result == HttpDownloader::OK && !contentDisposition.empty()) {
    filename = applyServerFilename(filename, contentDisposition);
  }

  if (result == HttpDownloader::OK) {
    Epub(filename, "/.crosspoint").clearCache();
    // Force the library to re-scan on next open (its dir-mtime cache
    // validation can't see root additions on FAT, nor a new folder).
    LibraryActivity::invalidateIndexCache();
    state = BrowserState::BROWSING;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  // OpenSearch's "{searchTerms}" (Atom), else the first RFC 6570 expression
  // of an OPDS 2 template: "{?query}" / "{&query}" become "?query=..." /
  // "&query=...", a plain "{query}" the bare term; other expressions drop.
  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) {
    url.replace(pos, placeholder.length(), urlEncode(query));
  } else {
    bool filled = false;
    for (size_t open = url.find('{'); open != std::string::npos; open = url.find('{', open)) {
      const size_t close = url.find('}', open);
      if (close == std::string::npos) break;
      std::string expr = url.substr(open + 1, close - open - 1);
      std::string replacement;
      if (!filled && !expr.empty()) {
        const char op = expr[0];
        if (op == '?' || op == '&') {
          const std::string name =
              expr.substr(1, expr.find(',') == std::string::npos ? std::string::npos : expr.find(',') - 1);
          replacement = std::string(1, op) + name + "=" + urlEncode(query);
        } else {
          replacement = urlEncode(query);
        }
        filled = true;
      }
      url.replace(open, close - open + 1, replacement);
      open += replacement.size();
    }
  }

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  consumeBack = consumeConfirm = true;
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
