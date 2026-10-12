#pragma once

// Builds the CLX1 index by walking the SD card once.
//
// The walk derives filename fallbacks and can read title, author, series/order, and subject metadata from
// EPUBs. Fresh metadata is reused from the previous index.
//
// Shape of the build, and why:
//
//   * ONE walk. Records go straight into a staging file in discovery order, so
//     nothing proportional to the library stays resident. The previous index is
//     matched through a sorted file with a small in-RAM fence, and every order
//     is sorted in bounded runs (LibrarySort.h) whose buffer is the only sizable
//     allocation after the walk: 32 KiB on the C3, the whole sort in PSRAM.
//   * Duplicate directory entries are dropped. A damaged FAT can hand the same
//     file out twice, and without this the shelf shows phantom books that
//     cannot be opened.
//   * Unreadable entries are skipped, never fatal.
//   * Install is write-then-rename, so an interrupted build leaves the previous
//     index untouched rather than a half-written one.

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

// Directory levels below the scan root that are walked. The cap exists because
// a corrupted FAT can contain a directory that contains itself, and an uncapped
// walk would never return. CrossInk uses 5; this fork's previous Library scan
// was an unbounded BFS, so the cap is looser here. Each level costs one small
// recursion frame (a closed directory handle, a name string, a few scalars).
inline constexpr int LIBRARY_MAX_DEPTH = 8;

// Duplicate identities remembered while one directory is enumerated. The
// fixed, fallible allocation is 8 KiB at this cap; unlike std::vector it cannot
// grow into abort() when a damaged or unusually flat directory is scanned.
inline constexpr uint16_t LIBRARY_MAX_DEDUP_KEYS = 1024;

// Why a build did not install a new index. Error covers I/O, allocation, and
// card faults; the other values let the UI say something more useful.
enum class BuildFailure : uint8_t {
  None,
  Error,
  Cancelled,
  TooManyBooks,
};

struct BuildStats {
  BuildFailure failure = BuildFailure::None;
  uint16_t books = 0;
  uint16_t folders = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint32_t walkMs = 0;
  // Reconciliation against the previous index. Their sum over a rebuild with no
  // card changes should be: unchanged == books, everything else zero.
  uint16_t unchanged = 0;  // same full path: keeps its place in "Recently added"
  uint16_t added = 0;      // matched nothing, not even by size
  uint16_t renamed = 0;    // matched a leftover entry by size alone
  uint16_t removed = 0;    // previous entry no book claimed
  uint16_t enriched = 0;   // took its title or author from the book rather than the filename
  uint16_t parsed = 0;     // EPUB metadata reads performed by this build
  uint16_t metadataReused = 0;
  uint16_t metadataCached = 0;  // EPUB metadata restored from library.clm instead of parsed
  bool indexReplaced = false;
  // Another scan could do better (the build failed, or ran short of memory for
  // a degradable step). The caller should not mark the index as current.
  bool retryAdvised = false;
  bool ranksDegraded = false;
  // Duplicate detection was incomplete: either its key buffer could not be
  // allocated (dedupAllocFailed, worth retrying) or one folder chain holds more
  // books than the tracker remembers (same result on every scan).
  bool dedupDegraded = false;
  bool dedupAllocFailed = false;
  bool arrivalDegraded = false;
};

enum class BuildPhase : uint8_t {
  Scanning,    // walking folders and reading book metadata; `books` counts what was found
  Organizing,  // sorting and writing the finished index
};

struct BuildProgress {
  BuildPhase phase = BuildPhase::Scanning;
  uint16_t books = 0;
};

// Optional hooks for a foreground build. Plain function pointers keep the
// library free of std::function; `context` is borrowed and passed back as-is.
struct BuildCallbacks {
  void* context = nullptr;
  // Polled between bounded units of build work. Returning true stops the build
  // and leaves the previous index installed.
  bool (*cancelRequested)(void* context) = nullptr;
  // Throttled to one call per LIBRARY_PROGRESS_INTERVAL_MS, plus one at each
  // phase change, because every call usually repaints an e-ink panel.
  void (*progress)(void* context, const BuildProgress& progress) = nullptr;
  // Fork addition. Called once for every directory the walk opens (including
  // ones that hold no book), with its packed FAT modification time. LibraryScan
  // records these so a later visit can validate the index with one open per
  // directory instead of walking the card again.
  void (*directoryVisited)(void* context, const char* path, uint32_t modifiedPacked) = nullptr;
};

inline constexpr uint32_t LIBRARY_CANCEL_POLL_MS = 50;
inline constexpr uint32_t LIBRARY_PROGRESS_INTERVAL_MS = 3000;

// Books one build can index. Sorting spills bounded runs to the card, so RAM
// does not limit this on any device; the format ceiling does.
uint16_t libraryBookLimit();

// Walk `rootPath`, write `/.crosspoint/library.clx`, and report what happened.
// The previous index, including its monotonic "recently added" counter, is read
// internally so callers cannot accidentally split one rebuild state across two
// file opens.
// `readMetadata` takes title, author, series/order, and tags from each EPUB
// (and title/author from each XTC). For EPUBs it stops the OPF parser at the
// end of <metadata>, before the manifest, without building the reader's spine,
// TOC, CSS, or section caches. Unchanged books reuse these values from the
// prior Library index, and interrupted scans resume from library.clm.
// `callbacks` may be null.
//
// Everything the build allocates is released before it returns: nothing stays
// resident. Whether the index is still current is NOT tracked here (CrossInk
// keeps an in-RAM dirty flag); this fork's LibraryScan persists that with the
// directory modification times reported through `directoryVisited`.
bool buildLibraryIndex(const char* rootPath, BuildStats& stats, bool readMetadata = false,
                       const BuildCallbacks* callbacks = nullptr);

// Live index path, shared by the builder and activity.
const char* libraryIndexPath();

}  // namespace library
