#pragma once

#include <cstddef>
#include <string>

class Epub;

// Shared BookFusion cover caching. The cover image is served by the BookFusion
// API (not embedded reliably in the EPUB), so it's downloaded from the API cover
// URL, converted to a grayscale thumbnail plus fit/crop cover BMPs, and stored in
// the book's cache directory. Used by both the download flow
// (BookFusionBrowserActivity) and the bulk metadata refresh
// (RefreshBookFusionMetadataActivity) so there is exactly one copy of this logic.
namespace BookFusionCoverCache {

// Downloads `coverUrl` (accepts protocol-relative "//..." and root-relative
// "/..." forms), then writes:
//   - <cache>/thumb_<coverHeight>.bmp   (grayscale, cropped thumbnail)
//   - <cache>/cover BMP (fit + crop variants) for the sleep screen
// Returns true when at least the thumbnail was produced. On success, if
// `outThumbPath` is non-null, the resolved thumb path is copied into it.
//
// WiFi must already be connected. Requires the epub's cache directory to exist
// (call epub.setupCacheDir() first if it might not).
bool refresh(const std::string& coverUrl, const Epub& epub, int coverHeight, char* outThumbPath = nullptr,
             size_t outThumbPathLen = 0);

// Split-phase variant for heap-constrained flows: `download` runs only the
// network fetch (raw image into the cache dir), `convert` runs only the
// JPEG/PNG→BMP conversions and removes the raw temp. This lets a caller lend
// the framebuffer to each phase's hungry subsystem in turn — a
// TlsFramebufferBorrow around download(), then a JpegScratchLease around
// convert() — without the two leases ever aliasing the same memory.
// refresh() == download() + convert().
bool download(const std::string& coverUrl, const Epub& epub);
bool convert(const Epub& epub, int coverHeight, char* outThumbPath = nullptr, size_t outThumbPathLen = 0);

}  // namespace BookFusionCoverCache
