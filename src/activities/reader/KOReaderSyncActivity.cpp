#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cassert>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SilentRestart.h"
#include "TlsFramebufferBorrow.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "clippings/ClippingSync.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HardcoverSync.h"
#include "util/WifiTimeSync.h"

namespace {
std::string documentHashFor(const std::string& epubPath, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(epubPath)
                                                 : KOReaderDocumentId::calculate(epubPath);
}

void syncTimeWithNTP() {
  // Delegates to the shared helper so the lastKnownValidTimestamp gets
  // persisted to APP_STATE — important so a later boot without NTP can still
  // render a sensible header date instead of falling back through readingDays.
  WifiTimeSync::attemptIfStale();
}
}  // namespace

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  LOG_DBG("KOSync", "WiFi connected, starting sync");

  // Keep the station fully awake for the short sync transaction: ESP32 modem
  // sleep can stall the socket for seconds and surface as an HTTP timeout
  // (upstream #3233). WiFi is torn down when this activity exits.
  WiFi.setSleep(false);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Sync time with NTP before making API requests
  syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

CrossPointPosition KOReaderSyncActivity::localPosition() const {
  CrossPointPosition pos = {currentSpineIndex, currentPage, totalPagesInSpine};
  if (currentParagraphIndex.has_value()) {
    pos.paragraphIndex = *currentParagraphIndex;
    pos.hasParagraphIndex = true;
  }
  if (currentVisibleTextOffset.has_value()) {
    pos.visibleTextOffset = *currentVisibleTextOffset;
    pos.hasVisibleTextOffset = true;
  }
  pos.hasResolvedSpineIndex = true;
  pos.hasMappedPage = true;
  return pos;
}

CrossPointPosition KOReaderSyncActivity::mapRemote(const KOReaderProgress& progress) {
  const KOReaderPosition koPos = {progress.progress, progress.percentage};
  CrossPointPosition mapped = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInSpine);
  ProgressMapper::refinePageFromLayout(epub, renderer, mapped);
  if (!mapped.hasMappedPage && progress.position.has_value()) {
    // The XPath could not be placed on a page; the crosspoint-sync position's
    // layout hints are the next best thing.
    if (auto rich = ProgressMapper::fromRichPosition(epub, *progress.position)) {
      ProgressMapper::refinePageFromLayout(epub, renderer, *rich);
      mapped = *rich;
    }
  }
  return mapped;
}

void KOReaderSyncActivity::performSync() {
  // Calculate document hash based on user's preferred method
  documentHash = documentHashFor(epubPath, KOREADER_STORE.getMatchMethod());
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }

  LOG_DBG("KOSync", "Document hash: %s", documentHash.c_str());
  const bool smart = direction == Direction::ASK && KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;

  if (KOREADER_STORE.getSyncClippings()) {
    {
      RenderLock lock(*this);
      statusMessage = tr(STR_SYNCING_CLIPPINGS);
    }
    requestUpdateAndWait();
    bool clippingsOk = false;
    {
      // Content offsets first: the lookup inflates chapters into the
      // framebuffer, which the TLS borrow below also claims.
      RenderLock lock(*this);
      if (clippingSync::resolveContentOffsets(renderer, *epub)) framebufferDirty = true;
    }
    {
      // One borrow for the whole exchange: it is a run of requests on one
      // connection with no rendering between them.
      TlsFramebufferBorrow borrow(renderer);
      clippingsOk = clippingSync::run(epubPath, epub->getTitle(), epub->getAuthor(), documentHash);
      framebufferDirty = borrow.active();
    }
    if (!clippingsOk) {
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = tr(STR_CLIPPING_SYNC_FAILED);
      }
      requestUpdate(true);
      return;
    }
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Fetch remote progress under the configured Document Matching method only.
  // Also reading the other method's record let a position saved for a
  // different copy of the file (same name, other edition; or same bytes,
  // other name) move this book somewhere that does not exist in it
  // (CrossInk 88af432d9).
  const auto result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // No remote progress on the server.
    if (direction == Direction::PUSH || smart) {
      // Nothing to compare; just upload local.
      performUpload();
      return;
    }
    if (direction == Direction::PULL) {
      // Nothing to pull. Surface a clear failure rather than the legacy
      // "offer to upload" prompt the user didn't ask for.
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = "No remote progress to pull";
      }
      requestUpdate(true);
      return;
    }
    // ASK: legacy "offer to upload" behaviour.
    {
      RenderLock lock(*this);
      state = NO_REMOTE_PROGRESS;
      hasRemoteProgress = false;
    }
    requestUpdate(true);
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  hasRemoteProgress = true;
  remotePosition = mapRemote(remoteProgress);

  // Calculate local progress in KOReader format (for display)
  const CrossPointPosition localPos = localPosition();
  localProgress = ProgressMapper::toKOReader(epub, localPos);

  // Explicit direction skips the comparison screen.
  if (direction == Direction::PULL) {
    setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber});
    finish();
    return;
  }
  if (direction == Direction::PUSH) {
    performUpload();
    return;
  }

  const ProgressComparison comparison =
      compareProgress(localPos, localProgress.percentage, remotePosition, remoteProgress.percentage);
  if (smart) {
    LOG_DBG("KOSync", "Smart decision %d: local %.4f, remote %.4f -> spine %d page %d", static_cast<int>(comparison),
            localProgress.percentage, remoteProgress.percentage, remotePosition.spineIndex, remotePosition.pageNumber);
    switch (comparison) {
      case ProgressComparison::Synchronized:
        esp_wifi_stop();
        {
          RenderLock lock(*this);
          state = ALREADY_SYNCED;
        }
        requestUpdate(true);
        return;
      case ProgressComparison::LocalAhead:
        performUpload();
        return;
      case ProgressComparison::RemoteAhead:
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber});
        finish();
        return;
      case ProgressComparison::Unknown:
        LOG_DBG("KOSync", "Smart sync could not compare; asking");
        break;
    }
  }

  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;
    // Default to the option that corresponds to the furthest progress
    selectedOption = comparison == ProgressComparison::LocalAhead ? 1 : 0;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // Convert current position to KOReader format
  const CrossPointPosition localPos = localPosition();
  const KOReaderPosition koPos = ProgressMapper::toKOReader(epub, localPos);

  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = koPos.xpath;
  progress.percentage = koPos.percentage;
  progress.progressIsGuess = !koPos.structural;
  // The client drops it again for a strict KOSync server.
  progress.position = ProgressMapper::toRichPosition(localPos, koPos);
  if (epub) {
    progress.metadata = KOReaderMetadata::fromBook(*epub);
  }

  const auto result = KOReaderSyncClient::updateProgress(progress);

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  esp_wifi_stop();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  // Connect to WiFi with popup feedback
  LOG_DBG("KOSync", "Connecting to WiFi with popup...");
  connectWifiWithPopup();
}

void KOReaderSyncActivity::onExit() {
  Activity::onExit();

  if (wifiActivated) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();

  if (framebufferDirty) {
    renderer.clearScreen();
    framebufferDirty = false;
  }

  // Use popup for simple status states, preserve background
  if (state == NO_CREDENTIALS) {
    char fullMessage[128];
    snprintf(fullMessage, sizeof(fullMessage), "%s\n\n%s", tr(STR_NO_CREDENTIALS_MSG), tr(STR_KOREADER_SETUP_HINT));
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), fullMessage);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), statusMessage.c_str());
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    // Use full screen for detailed comparison
    renderer.clearScreen();

    // Show comparison
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    // Get chapter names from TOC
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        (localTocIndex >= 0) ? epub->getTocItem(localTocIndex).title
                             : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 185, remoteChapterStr);
    char remotePageStr[64];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
             remoteProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 210, remotePageStr);

    if (!remoteProgress.device.empty()) {
      char deviceStr[64];
      snprintf(deviceStr, sizeof(deviceStr), tr(STR_DEVICE_FROM_FORMAT), remoteProgress.device.c_str());
      renderer.drawText(UI_10_FONT_ID, 20, 235, deviceStr);
    }

    // Local progress - chapter and page
    renderer.drawText(UI_10_FONT_ID, 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 295, localChapterStr);
    char localPageStr[64];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1, totalPagesInSpine,
             localProgress.percentage * 100);
    renderer.drawText(UI_10_FONT_ID, 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    // Apply option
    if (selectedOption == 0) {
      renderer.fillRect(0, optionY - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    // Upload option
    if (selectedOption == 1) {
      renderer.fillRect(0, optionY + optionHeight - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    // Bottom button hints
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    char fullMessage[128];
    snprintf(fullMessage, sizeof(fullMessage), "%s\n\n%s", tr(STR_NO_REMOTE_MSG), tr(STR_UPLOAD_PROMPT));
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), fullMessage);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE || state == ALREADY_SYNCED) {
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC),
                                   state == ALREADY_SYNCED ? tr(STR_ALREADY_SYNCED) : tr(STR_UPLOAD_SUCCESS));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    char fullMessage[256];
    snprintf(fullMessage, sizeof(fullMessage), "%s\n\n%s", tr(STR_SYNC_FAILED_MSG), statusMessage.c_str());
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), fullMessage);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == ALREADY_SYNCED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;  // Wrap around among 2 options
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        // Wifi will be turned off in onExit()
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber});
        finish();
      } else if (selectedOption == 1) {
        // Upload local progress
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Calculate hash if not done yet
      if (documentHash.empty()) {
        documentHash = documentHashFor(epubPath, KOREADER_STORE.getMatchMethod());
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }
}

void KOReaderSyncActivity::connectWifiWithPopup() {
  // Show connecting popup
  {
    RenderLock lock(*this);
    UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "Connecting to WiFi...");
    if (SETTINGS.darkMode) renderer.invertScreen();
    renderer.displayBuffer();
  }

  // Load WiFi credentials
  WIFI_STORE.loadFromFile();

  // Try to auto-connect to the last known network
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (lastSsid.empty()) {
    // No saved networks, show error popup
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC),
                                     "No WiFi networks configured.\nPlease set up WiFi in Settings first.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    // Wait briefly to show message, then exit
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  const auto* cred = WIFI_STORE.findCredential(lastSsid);
  if (!cred) {
    // Network found but no credentials
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC),
                                     "WiFi credentials not found.\nPlease reconnect in Settings.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    // Wait briefly to show message, then exit
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Attempt connection
  LOG_DBG("KOSync", "Attempting to connect to WiFi: %s", lastSsid.c_str());

  HardcoverSync::preempt();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (cred->password.empty()) {
    WiFi.begin(cred->ssid.c_str());
  } else {
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  }

  // Wait for connection with timeout
  int attempts = 0;
  const int maxAttempts = 100;  // 10 seconds

  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    attempts++;

    // Update popup every second
    if (attempts % 10 == 0) {
      RenderLock lock(*this);
      char statusMsg[64];
      snprintf(statusMsg, sizeof(statusMsg), "Connecting to WiFi...\n(%d/%d)", attempts / 10, maxAttempts / 10);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), statusMsg);
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("KOSync", "WiFi connected successfully");

    // Show success popup briefly
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC), "WiFi connected!");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Proceed with sync
    onWifiSelectionComplete(true);
  } else {
    LOG_DBG("KOSync", "WiFi connection failed");

    // Show failure popup
    {
      RenderLock lock(*this);
      UITheme::drawSyncProgressPopup(renderer, tr(STR_KOREADER_SYNC),
                                     "WiFi connection failed.\nPlease check your settings.");
      if (SETTINGS.darkMode) renderer.invertScreen();
      renderer.displayBuffer();
    }

    // Wait to show message, then exit
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}
