#pragma once

#include <I18n.h>

#include <string>

#include "Dictionary.h"

class GfxRenderer;

// One dictionary session for one visit to the reader: opens SETTINGS.dictionaryName
// once and remembers whether its sidecars are stale, so looking up a second word
// costs a lookup rather than another open + validate.
//
// Split into prepare() / needsIndexing() / run() on purpose: the caller has to
// paint a busy popup BEFORE run() blocks on the SD card, and the message differs
// ("Indexing dictionary..." is a multi-second first pass, "Looking up..." is not).
class DictionaryLookup {
 public:
  struct Outcome {
    bool found = false;
    std::string headword;
    std::string definition;
    bool html = false;
    // Popup text when !found — distinguishes a genuine miss from a decompression,
    // low-memory or read failure, all of which look identical to the user
    // otherwise and mean very different things.
    StrId message = StrId::STR_DICT_NOT_FOUND;
  };

  // Open the configured dictionary if it is not already open. False when no
  // dictionary is set or the folder cannot be opened. Cheap after the first call.
  bool prepare();

  // True when run() must build the .qidx / .sidx sidecars before it can search.
  bool needsIndexing() const { return needsIndex; }

  // Build the index if stale, then look the word up.
  //
  // Borrows the framebuffer as the inflate window for the duration (see
  // InflateScratchLease): a .dict.dz entry needs 32KB CONTIGUOUS, which is the
  // allocation that fails first on the C3 — after one BLE cycle the largest free
  // block sits near 25KB for the rest of the session. The framebuffer is
  // allocated at boot and never moves, so borrowing it makes the lookup immune to
  // that fragmentation instead of merely unlikely to hit it, and the BLE stack
  // can stay up. On a PSRAM board the lease compiles out and this is a plain
  // malloc, which is never in doubt there.
  //
  // CALLER CONTRACT: the lease destroys the framebuffer's contents, so the caller
  // MUST fully repaint after this returns — never draw a popup straight over it.
  Outcome run(GfxRenderer& renderer, const char* word);

 private:
  Dictionary dict;
  bool openAttempted = false;
  bool openOk = false;
  bool needsIndex = false;
};
