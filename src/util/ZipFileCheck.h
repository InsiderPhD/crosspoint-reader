#pragma once

/**
 * Cheap structural completeness test for a ZIP container (EPUB, XTC, plugin
 * bundles). A ZIP ends with an End Of Central Directory record; a transfer
 * that stopped early never has one, whatever the server said about its size.
 */
namespace ZipFileCheck {

// True if `path` starts with a local-file-header signature and an EOCD
// signature appears in the last 64 KB + 22 bytes (the maximum comment span).
// Reads at most ~66 KB in 256-byte windows from the stack; no heap.
bool looksComplete(const char* path);

}  // namespace ZipFileCheck
