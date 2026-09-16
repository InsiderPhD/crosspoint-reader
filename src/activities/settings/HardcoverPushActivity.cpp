#include "HardcoverPushActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HardcoverTokenStore.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/TlsFramebufferBorrow.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HardcoverSync.h"

void HardcoverPushActivity::collectPending(const bool includeUpToDate) {
  const auto& books = READING_STATS.getBooks();
  pending.clear();
  pending.reserve(books.size());
  for (size_t i = 0; i < books.size() && i <= UINT16_MAX; i++) {
    if (HardcoverSync::isEligible(books[i]) && (includeUpToDate || !HardcoverSync::isUpToDate(books[i]))) {
      pending.push_back(static_cast<uint16_t>(i));
    }
  }
  LOG_DBG("HCS", "%u of %u stats books need a Hardcover push", static_cast<unsigned>(pending.size()),
          static_cast<unsigned>(books.size()));
}

void HardcoverPushActivity::onEnter() {
  Activity::onEnter();
  updated = notFound = failed = 0;
  failMessage = nullptr;
  forceAll = false;

  if (!HC_TOKEN_STORE.hasToken()) {
    state = NO_TOKEN;
  } else {
    collectPending(false);
    state = pending.empty() ? NOTHING_TO_PUSH : CONFIRM;
  }
  requestUpdate();
}

void HardcoverPushActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
}

void HardcoverPushActivity::pushAll() {
  // One static frame for the whole run, then the rails go off (see render()):
  // e-ink holds "Pushing N books" with no power while the requests run.
  requestUpdateAndWait();

  // Lend the framebuffer to TLS for the entire batch, as the download flows do.
  // Nothing may render until it ends, so the run happens inside this single
  // loop() pass: a borrow held across passes deadlocks the activity manager
  // (it takes the same render lock to process a finish()). The loop watchdog is
  // fed per response chunk by the client.
  const auto& books = READING_STATS.getBooks();
  const char* stopMessage = nullptr;
  {
    TlsFramebufferBorrow borrow(renderer);
    for (const uint16_t bookIndex : pending) {
      if (bookIndex >= books.size()) continue;
      if (forceAll) {
        HardcoverSync::forgetSyncState(books[bookIndex]);
      }
      const auto err = HardcoverSync::pushBook(books[bookIndex]);
      if (err == HardcoverSyncClient::OK) {
        updated++;
      } else if (err == HardcoverSyncClient::NOT_FOUND) {
        notFound++;
      } else if (err == HardcoverSyncClient::SERVER_ERROR || err == HardcoverSyncClient::JSON_ERROR) {
        failed++;
      } else {
        // Token, rate limit or network: every remaining book would fail the
        // same way, so stop here.
        if (err == HardcoverSyncClient::RATE_LIMITED) {
          stopMessage = tr(STR_HARDCOVER_RATE_LIMITED);
        } else if (err == HardcoverSyncClient::NETWORK_ERROR) {
          stopMessage = tr(STR_HARDCOVER_NETWORK_FAILED);
        } else {
          stopMessage = tr(STR_HARDCOVER_TOKEN_REJECTED);
        }
        break;
      }
    }
  }

  {
    RenderLock lock(*this);
    failMessage = stopMessage;
    state = stopMessage ? FAILED : DONE;
  }
  requestUpdate();
}

void HardcoverPushActivity::startPush() {
  state = CONNECTING;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             failMessage = tr(STR_WIFI_CONN_FAILED);
                             state = FAILED;
                             requestUpdate();
                             return;
                           }
                           // Glyph cache re-warms on the next paint; TLS wants the heap now.
                           if (auto* fcm = renderer.getFontCacheManager()) {
                             fcm->clearCache();
                           }
                           state = PUSHING;
                         });
}

void HardcoverPushActivity::loop() {
  switch (state) {
    case CONFIRM:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        startPush();
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
      }
      return;

    case NOTHING_TO_PUSH:
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        collectPending(true);
        if (!pending.empty()) {
          forceAll = true;
          startPush();
        }
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        finish();
      }
      return;

    case PUSHING:
      pushAll();
      return;

    case CONNECTING:
      return;

    case NO_TOKEN:
    case DONE:
    case FAILED:
      if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
          mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        finish();
      }
      return;
  }
}

void HardcoverPushActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = pageHeight / 2 - lineHeight;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PUSH_TO_HARDCOVER));

  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = "";
  char line[96];

  switch (state) {
    case NO_TOKEN:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_HARDCOVER_NO_TOKEN), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + 10, tr(STR_HARDCOVER_TOKEN_HINT));
      break;

    case NOTHING_TO_PUSH:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_HARDCOVER_UP_TO_DATE), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + 10, tr(STR_HARDCOVER_PUSH_AGAIN_HINT));
      confirmLabel = tr(STR_HARDCOVER_PUSH_AGAIN);
      break;

    case CONFIRM:
      snprintf(line, sizeof(line), tr(STR_HARDCOVER_CONFIRM_FORMAT), static_cast<unsigned>(pending.size()));
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, line, true, EpdFontFamily::BOLD);
      backLabel = tr(STR_CANCEL);
      confirmLabel = tr(STR_CONFIRM);
      break;

    case CONNECTING:
      break;

    case PUSHING:
      // Static: nothing repaints until the whole batch is done.
      snprintf(line, sizeof(line), tr(STR_HARDCOVER_PUSHING_FORMAT), static_cast<unsigned>(pending.size()));
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, line, true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + 10, tr(STR_DOWNLOAD_WAIT));
      backLabel = "";
      break;

    case DONE:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_HARDCOVER_DONE), true, EpdFontFamily::BOLD);
      snprintf(line, sizeof(line), tr(STR_HARDCOVER_RESULT_FORMAT), updated, notFound, failed);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + 10, line);
      break;

    case FAILED:
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, failMessage ? failMessage : tr(STR_HARDCOVER_NETWORK_FAILED),
                                true, EpdFontFamily::BOLD);
      if (updated + notFound + failed > 0) {
        snprintf(line, sizeof(line), tr(STR_HARDCOVER_RESULT_FORMAT), updated, notFound, failed);
        renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + 10, line);
      }
      break;
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (SETTINGS.darkMode) renderer.invertScreen();
  // The PUSHING frame is followed by the whole blocking batch (pushAll waits on
  // this very render). Collapse the EPD analog rails for it, as
  // BookFusionSyncActivity does: holding the charge pump powered through
  // WiFi TX peaks on the shared 3.3V rail is what ghosted the panel on sync
  // timeouts. The result paint wakes the panel and the display driver promotes
  // it to a HALF refresh, which also scrubs any residue.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH, /*powerOffAfter=*/state == PUSHING);
}
