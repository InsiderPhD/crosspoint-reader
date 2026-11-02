#pragma once

#include <KOReaderSyncClient.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "activities/Activity.h"

// File Transfer > Sync Books: KOReader / CrossPoint Sync progress for every
// EPUB on the card that has a saved position, in one pass, without opening
// any of them (CrossInk's "Sync Books").
//
// On a CrossPoint or Other server it reads only what changed remotely since
// the last run (the progress change feed, cursor kept per server and account)
// and uploads only books whose position moved here since they were last
// synced, eight per request. A strict KOSync server, or one without those
// routes, gets the classic walk: one GET per book, one PUT per changed book.
//
// Smart sync keeps the further position. Ask on conflicts never guesses: a
// book whose server position differs is left alone and counted, to be synced
// from inside the book where the choice can be shown.
//
// Heap: runs outside the reader, so no book is open and Bluetooth is already
// off (it only runs in the reader). Resident for the run: one entry per synced
// book (path + 33-byte id, ~100 bytes). Per step, never together with a TLS
// request: one Epub for mapping, freed before the next request. Each request
// runs inside a TlsFramebufferBorrow, as every C3 TLS call must.
class SyncBooksActivity final : public Activity {
 public:
  SyncBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SyncBooks", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Running; }
  bool skipLoopDelay() override { return state == State::Running; }

 private:
  enum class State : uint8_t { NoCredentials, Connecting, Running, Done, Failed };

  struct Book {
    std::string path;
    char hash[33] = {};
    bool handled = false;   // pulled, in sync, or a conflict: nothing to upload
    bool conflict = false;  // Ask on conflicts: server position differs
  };

  // What the server holds for one book, from the feed or a per-book GET.
  struct Remote {
    float percentage = 0.0f;
    std::string progress;
    std::optional<KOReaderRichPosition> position;
    int64_t timestamp = 0;
  };

  State state = State::Connecting;
  const char* phase = "";
  const char* failMessage = nullptr;
  std::vector<Book> books;
  int processed = 0;
  int total = 0;
  unsigned uploaded = 0;
  unsigned pulled = 0;
  unsigned unchanged = 0;
  unsigned conflicts = 0;
  unsigned failed = 0;
  bool batchUnsupported = false;
  int lastRepaint = -1;

  void startWifi();
  void runSync();
  bool collectBooks();
  // Change feed. False with failMessage set on a hard error; sets
  // feedUsed=false when the server has no feed (fall back to the walk).
  bool readChangeFeed(bool& feedUsed);
  bool walkBooks();
  bool uploadChanged();
  void handleRemote(Book& book, const Remote& remote);
  // Upload books[indices] one by one (strict KOSync, or no batch route).
  bool uploadSingle(size_t index);
  // One queued batch item: what to record in the book's sync sidecar once the
  // server accepts it.
  struct BatchEntry {
    size_t index = 0;
    float percentage = 0.0f;
    int spine = 0;
    int page = 0;
    int pages = 0;
  };
  bool flushBatch(KOReaderProgressBatch& batch, std::vector<BatchEntry>& entries);
  // Fill `out` with this book's saved position as an upload. False when the
  // book has no usable saved position.
  bool buildUpload(const Book& book, KOReaderProgress& out, int& spine, int& page, int& pages) const;
  void recordSynced(const Book& book, float percentage, int spine, int page, int pages, int64_t timestamp) const;
  void step(const char* newPhase, int done, int count);
};
