#pragma once

#include <Epub.h>

// Progress writes that are needed outside EpubReaderActivity itself.
//
// Screens that move the reader somewhere else while the reader is off-screen
// (nearby position sync, which silent-reboots back into the book) cannot hand a
// SyncResult back: the activity manager runs onExit() -- and therefore the
// reboot -- before the popped activity's result handler. They have to land the
// new position on the SD card themselves.
namespace EpubReaderUtils {

// Writes progress.bin for this book and refreshes its Recent Books percentage.
// Returns false if the cache file could not be opened.
bool saveProgress(Epub& epub, int spineIndex, int currentPage, int pageCount);

}  // namespace EpubReaderUtils
