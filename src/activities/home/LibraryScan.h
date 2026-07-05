#pragma once

#include <string>
#include <vector>

// Whole-SD book enumeration, shared by any activity that needs the full library
// list (LibraryActivity, GroupBrowserActivity, the dev-mode metadata recache).
//
// Enumeration is file-driven: the per-book cache dir is named epub_<hash> and the
// hash isn't reversible, so the only way to know which books exist is to walk the
// card. A bounded BFS finds every .epub / .xtc; the result is persisted to
// /.crosspoint/library_index.bin and validated against each scanned directory's
// FAT mtime, so repeated opens skip the scan until a file is added/removed/moved.
namespace LibraryScan {

// Fills `outPaths` with every .epub / .xtc path on the SD card. Uses the persisted
// index when it is present and every recorded directory mtime still matches;
// otherwise runs a full BFS and rewrites the index. `outPaths` is cleared first.
void enumerateBooks(std::vector<std::string>& outPaths);

// Delete the persisted index so the next enumerateBooks() does a full re-scan.
void invalidateIndex();

}  // namespace LibraryScan
