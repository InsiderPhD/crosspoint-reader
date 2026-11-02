#pragma once

#include <cstdint>

class GfxRenderer;

// Delayed "Loading" popup for opening a book whose caches are already built
// (CrossInk c781616a5). ReaderActivity skips its up-front popup for those, since
// a fast open would pay an extra panel refresh for nothing. But a cached open
// can still be slow (a big spine page-count scan, an SD font swap, a section
// whose cache went stale), and then the previous screen sits there looking
// frozen. So the open starts this timer, and each loading stage asks
// showIfDue(): once DELAY_MS has passed the popup is drawn (once), otherwise
// nothing happens and a fast open shows no popup at all.
//
// Passed by value from ReaderActivity to the reader. Only call showIfDue()
// between loading stages, while the framebuffer is not lent out and before the
// page itself is composed: the popup is drawn into the framebuffer.
class BookLoadingFeedback {
  uint32_t startedAt = 0;
  bool pending = false;

 public:
  static constexpr uint32_t DELAY_MS = 300;

  void start(const uint32_t now) {
    startedAt = now;
    pending = true;
  }
  // Something else (the Indexing popup, an error screen, the page) is about to
  // show: never draw the Loading popup after this.
  void finish() { pending = false; }
  bool isDue(const uint32_t now) const { return pending && static_cast<uint32_t>(now - startedAt) >= DELAY_MS; }
  // Draws the popup and pushes it to the panel if due. True when it drew.
  bool showIfDue(GfxRenderer& renderer);
};
