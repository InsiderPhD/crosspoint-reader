#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// A plugin put a file on the card (catalog download, /api/fetch,
// /api/plugin-fs): drop a stale reading cache for it and, for a book the
// Library lists (.epub / .xtc outside hidden folders), mark the Library index
// stale. A download does not change the SD root's FAT mtime, so the index
// cannot notice the new book itself (upstream CrossPoint #3997).
void noteBookInstalled(const std::string& path);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);

#endif  // CROSSPOINT_SD_PLUGINS
