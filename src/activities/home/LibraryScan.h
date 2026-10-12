#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace library {
struct BuildCallbacks;
}

// Whole-SD book enumeration, shared by any activity that needs the full library
// list (LibraryActivity, GroupBrowserActivity, the dev-mode metadata recache).
//
// Enumeration is file-driven: the per-book cache dir is named epub_<hash> and the
// hash isn't reversible, so the only way to know which books exist is to walk the
// card. Two persisted forms of that walk live under /.crosspoint:
//
//   library_index.bin  the original path list ("LIDX"): every .epub / .xtc path
//                      plus each scanned directory's FAT mtime. Loaded whole into
//                      a std::vector<std::string>, so its RAM grows with the card.
//   library.clx        the CLX1 Library index (lib/LibraryIndex): titles,
//                      authors, tags, series and five sort orders, read by seeks
//                      with nothing per-book resident. library.clx.dirs records
//                      the directory mtimes it was built against.
//
// Both are validated the same way: a directory whose FAT mtime changed means a
// file was added, removed or moved, and the form is rebuilt on next use.
// SETTINGS.libraryUseIndex selects the CLX1 form for the screens that can read
// it directly; with it off, nothing here touches library.clx.
namespace LibraryScan {

// Fills `outPaths` with every .epub / .xtc path on the SD card. `outPaths` is
// cleared first. Sources, in order: the LIDX path list when its directory
// mtimes still match; the CLX1 index when it is enabled and current (paths are
// streamed out of it, no card walk); otherwise a full BFS that rewrites LIDX.
// Order is unspecified; callers sort.
void enumerateBooks(std::vector<std::string>& outPaths);

// Mark both persisted forms stale so the next use re-scans the card. Call after
// adding or removing a book file. library.clx itself is kept: the rebuild
// reuses every unchanged book's metadata from it.
void invalidateIndex();

// --- CLX1 index ---------------------------------------------------------------

// SETTINGS.libraryUseIndex.
bool indexEnabled();

// True when library.clx exists, validates, and every directory recorded in
// library.clx.dirs still has the mtime it had when the index was built. Costs
// one open per directory; nothing is loaded.
bool indexIsCurrent();

enum class IndexRefresh : uint8_t {
  Ok,            // the index on the card is current (rebuilt, or found unchanged)
  Cancelled,     // the user stopped the scan; the previous index, if any, is untouched
  Failed,        // I/O or allocation failure; the previous index, if any, is untouched
  TooManyBooks,  // more books than the format holds; the previous index is untouched
};

// Walk the card and rebuild library.clx (flat memory; see LibraryBuilder.h),
// reading EPUB/XTC metadata for new or changed books and reusing the rest.
// `ui` may be null; its cancelRequested/progress hooks are forwarded (its
// directoryVisited hook is ignored, this function supplies its own). Blocks the
// calling task for the whole scan and yields to the scheduler throughout.
// Everything it allocates is freed before it returns.
IndexRefresh refreshIndex(const library::BuildCallbacks* ui);

// Path of the live CLX1 index, for LibraryIndexFile::open().
const char* indexPath();

}  // namespace LibraryScan
