#pragma once

#include <cstdint>

class GfxRenderer;

// Composites a sleep overlay image onto whatever the framebuffer already holds
// (the last page, for the Page Overlay sleep screen). Nothing here clears or
// refreshes the screen: the caller owns the paint.
//
// Both decoders stream one source row at a time straight into the framebuffer,
// so no image-sized buffer ever exists. See SleepOverlay.cpp for what each one
// allocates.
namespace SleepOverlay {

enum class Result : uint8_t {
  Drawn,     // the overlay is in the framebuffer
  NotFound,  // no such file; nothing was touched
  Failed,    // unusable file or not enough memory; logged
};

// PNG: pixels whose alpha is below 50% are skipped, so the page shows through.
// The rest are drawn in their own tone, ordered-dithered to black and white.
// Images larger than the screen are scaled down to fit; smaller ones are
// centred. A Failed result may leave a partial overlay behind only when the
// file itself is corrupt part-way through; a memory shortfall fails before
// the first pixel.
Result drawPng(GfxRenderer& renderer, const char* path);

// BMP: white is transparent, everything darker is drawn black.
Result drawBmp(GfxRenderer& renderer, const char* path);

}  // namespace SleepOverlay
