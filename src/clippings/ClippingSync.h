#pragma once

#include <string>

// Two-way clipping sync over the crosspoint-sync server's clippings extension
// (GET/PUT {base}/api/v1/clippings/{documentHash}), the same protocol upstream
// CrossPoint speaks, so clips move between this firmware and upstream devices.
class Epub;
class GfxRenderer;

namespace clippingSync {
// Find the content offsets (start_offset/end_offset) of this book's clips that
// have no verdict yet, by searching each clip's text in its chapter XHTML.
// Inflates chapters with the framebuffer as scratch, so it must run BEFORE the
// sync opens TLS and while nothing renders. No-op unless Sync Clippings is on.
// Returns true when the framebuffer was used: its contents are then garbage.
bool resolveContentOffsets(GfxRenderer& renderer, const Epub& epub);

// No-op returning true unless Sync Clippings is on in KOReader Sync settings.
// Returns false on an enabled sync that failed (server lacks the extension,
// network, memory, SD). Works on the store in place when it already holds this
// book (the reader keeps it loaded); otherwise loads it with title/author for
// the file header and unloads it before returning.
// On the C3, run it inside a TlsFramebufferBorrow: the TLS session needs the
// framebuffer to leave room for the response buffer.
bool run(const std::string& bookPath, const std::string& title, const std::string& author,
         const std::string& documentHash);
}  // namespace clippingSync
