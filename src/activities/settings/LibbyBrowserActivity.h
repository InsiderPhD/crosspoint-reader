#pragma once

#include <LibbyClient.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

/**
 * Browse and send the user's Libby loans to the reader.
 *
 * Deliberately shaped like BookFusionBrowserActivity -- same list, same frozen
 * info card during the transfer, same states -- minus the category/shelf menu.
 * There is nothing to filter: a library card allows a handful of concurrent
 * loans and they all fit in memory, so the list is fetched once and paginated
 * by drawList itself.
 *
 * Linking a Libby account and authorising the reader for protected content both
 * stay in the web UI (File Transfer -> Join Network -> /libby). They are
 * one-time, and they need crypto this firmware does not compile in. This
 * activity therefore refuses with a pointer to the web UI rather than trying to
 * set anything up itself.
 */
class LibbyBrowserActivity final : public Activity {
 public:
  explicit LibbyBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LibbyBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  // Tap dispatch only covers the loan list; every other state keeps the global
  // tap-is-Confirm injection.
  bool handlesDirectTouch() const override { return state == BROWSING; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    // What to do with the highlighted loan. A menu rather than a second button
    // because the four hint slots are already spoken for, and it leaves room
    // for returning a loan later without another redesign.
    LOAN_ACTIONS,
    SENDING,
    SEND_COMPLETE,
    ERROR,
  };

  enum LoanAction { ACTION_SEND = 0, ACTION_RENEW, ACTION_COUNT };

  State state = WIFI_SELECTION;
  ButtonNavigator buttonNavigator;

  LibbyLoanList loans;  // ~5 KB, fetched once per visit
  int selectedIndex = 0;
  int selectedAction = ACTION_SEND;

  char sendTitle[72] = {};
  char sendAuthor[48] = {};
  // Phase label on the Sending screen -- "Authenticating…", "Preparing loan…",
  // "Downloading…" -- so a multi-minute fulfilment doesn't look wedged.
  char sendStatus[40] = {};
  size_t sendDone = 0;   // bytes transferred so far
  size_t sendTotal = 0;  // 0 until the server declares a length

  // The Sending screen repaints on every progress tick. A full render() would
  // wipe the framebuffer and redraw the whole card; in single-buffer mode the
  // buffer persists, so once the static part is painted only the status/progress
  // band is repainted. Same trick BookFusionBrowserActivity uses, for the same
  // reason: SD and heap are both busy during the transfer.
  bool sendScreenPainted = false;
  int sendStatusY = 0;
  void drawSendDynamic(int statusY);

  // Wide enough for a two-part message: a renewal can succeed on Libby and
  // still fail to refresh the licence on the card, and both halves matter.
  char errorMsg[224] = {};
  // The completion screen serves both actions, so it carries its own headline
  // rather than assuming a download happened.
  char completeMsg[96] = {};

  // List body of BROWSING. Shared by render() and loop()'s tap hit-testing so
  // the two cannot disagree.
  Rect listRect() const;

  void onWifiSelectionComplete(bool success);
  void loadLoans();
  void sendSelectedLoan();
  void renewSelectedLoan();
  // Cache OverDrive's cover artwork for a loan that is now on the card.
  //
  // Library books are Adobe-encrypted, so the artwork inside the EPUB is not
  // dependably readable and these titles otherwise sit in the library with a
  // blank cover. Best-effort and never fatal -- it does its own framebuffer
  // borrow, so it must be called with none already held.
  void cacheLoanCover(const LibbyLoan& loan, const char* bookPath);
  void runAction();
  void fail(const char* message);
  // Shared prologue for both actions: freeze the info card on the panel before
  // the framebuffer is lent to TLS.
  void beginBusyCard(const LibbyLoan& loan, const char* status);

  // Progress trampoline handed to AdeptClient.
  static bool onProgress(void* ctx, const char* phase, size_t done, size_t total);
};
