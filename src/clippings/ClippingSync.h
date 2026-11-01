#pragma once

#include <string>

// Two-way clipping sync over the crosspoint-sync server's clippings extension
// (GET/PUT {base}/api/v1/clippings/{documentHash}), the same protocol upstream
// CrossPoint speaks, so clips move between this firmware and upstream devices.
namespace clippingSync {
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
