#pragma once

#include <string>

// "Sync Book" from a book's context menu (Home, Library, Tags/Authors, File
// Browser). The reader's own Sync action already does the whole job: it maps
// the saved position, runs the KOReader / CrossPoint Sync or BookFusion flow
// with the user's Sync Behavior, syncs clippings when turned on, and ends in
// the silent reboot that lands back in the book. So the launcher opens the
// book with a one-shot request and the reader starts that action as soon as
// its first page is on screen (CrossInk resumes in the book the same way).
namespace SyncBookLauncher {

// True for an EPUB that has somewhere to sync to: a KOReader/CrossPoint Sync
// account, or a BookFusion book with a linked account. One small sidecar read
// for the BookFusion id; call it when a menu opens, not every frame.
bool canSync(const std::string& path);

// Open the book and sync it once its first page renders.
void start(const std::string& path);

}  // namespace SyncBookLauncher
