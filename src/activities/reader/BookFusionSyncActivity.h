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
  // ASK fetches BookFusion's position, shows it next to the device's and lets
  // the user pick a direction; this is what the reader menu's Sync row uses.
  // PUSH / PULL skip the question.
  enum class Direction { ASK, PUSH, PULL };

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
  enum State { SYNCING, CHOOSING, RESULT_OK, RESULT_FAILED };

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

  // One side of the comparison, drawn as a bordered card with a progress bar.
  struct ProgressCard {
    bool shown = false;
    bool ahead = false;       // further on than the other side (PULL only)
    float percentage = 0.0f;  // 0-100, of the whole book
    std::string chapter;
    std::string pageText;     // "Page 3/12"; empty when the page isn't known
    std::string countsLine;   // "3 bookmarks | 4 highlights" held on that side
    std::string updatedLine;  // who wrote it and when; empty when unknown
  };
  ProgressCard remoteCard;
  ProgressCard localCard;
  // CHOOSING (ASK only): 0 = apply BookFusion's position, 1 = upload ours.
  int selectedOption = 0;
  bool canApplyRemote = false;  // false when BookFusion has no position yet
  // The fetched remote position. PULL records it as the sync baseline at once;
  // ASK holds it here until the user actually applies it.
  BookFusionPosition fetchedRemote;
  bool fetchedRemoteIsOurs = false;
  bool baselinePending = false;

  // For PULL: where to send the reader.
  int resolvedSpineIndex = 0;
  float resolvedIntra = 0.0f;
  float resolvedPercentage = 0.0f;

  std::string chapterNameForSpine(int spineIndex) const;
  // lastSyncIso: server timestamp of this device's latest sync, "" if none.
  void fillLocalCard(float percentage, int bookmarks, const char* lastSyncIso);

  void performSync();
  void performPush();
  void performPull();
  void savePullBaseline();
  void finishCancelled();
};
