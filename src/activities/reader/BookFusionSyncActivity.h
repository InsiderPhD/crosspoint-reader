#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "BookFusionSyncClient.h"
#include "activities/Activity.h"

/**
 * Full-screen, direction-explicit BookFusion sync.
 *
 * Mirrors the KOReaderSyncActivity flow so the two sync paths feel consistent:
 *   - Launched by the reader menu's Sync: Push / Pull entries (after the
 *     standard WifiSelectionActivity has connected WiFi).
 *   - Renders status updates full-screen with a header (not as popups on top
 *     of the EPUB).
 *   - For PULL, returns a SyncResult with the resolved spineIndex and
 *     intraSpineProgress; the reader does the page jump via pendingPercentJump.
 *
 * The long-press Confirm quick-sync still uses
 * EpubReaderActivity::performBookFusionSync (popup-style) — that path is
 * intentionally unchanged.
 */
class BookFusionSyncActivity final : public Activity {
 public:
  enum class Direction { PUSH, PULL };

  BookFusionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub,
                         uint32_t bookId, int currentSpineIndex, int currentPage, int totalPages, Direction direction)
      : Activity("BookFusionSync", renderer, mappedInput),
        epub(epub),
        bookId(bookId),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPages(totalPages),
        direction(direction) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool preventAutoSleep() override { return state == SYNCING; }
  bool skipLoopDelay() override { return state == SYNCING; }

 private:
  enum State { SYNCING, RESULT_OK, RESULT_FAILED };

  std::shared_ptr<Epub> epub;
  uint32_t bookId;
  int currentSpineIndex;
  int currentPage;
  int totalPages;
  Direction direction;

  State state = SYNCING;
  std::string statusMessage;

  // Set before painting the wait screen that immediately precedes a blocking
  // network call, so render() collapses the EPD analog rails (turnOffScreen)
  // for the duration of that call. Leaving the charge pump powered alongside
  // WiFi TX spikes during a sync timeout was causing severe ghosting. Consumed
  // (and cleared) by render(); the post-network paint then wakes from off and
  // is auto-promoted to a HALF refresh, which also scrubs any residue.
  bool powerDownAfterRender = false;

  // Result-screen content. Filled in by performPush / performPull and rendered
  // when state transitions to RESULT_OK / RESULT_FAILED.
  std::string resultTitle;
  std::string resultDetail;
  // Optional structured details — populated for both PUSH and PULL so the
  // user sees chapter name + page numbers, not just a percentage.
  bool hasRemoteDetails = false;
  std::string remoteChapter;
  std::string remotePageLine;
  std::string remotePercentLine;
  bool hasLocalDetails = false;
  std::string localChapter;
  std::string localPageLine;
  std::string localPercentLine;

  // For PULL: where to send the reader.
  int resolvedSpineIndex = 0;
  float resolvedIntra = 0.0f;
  float resolvedPercentage = 0.0f;

  std::string chapterNameForSpine(int spineIndex) const;

  void performSync();
  void performPush();
  void performPull();
};
