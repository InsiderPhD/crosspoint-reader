#pragma once

#include <string>

class GfxRenderer;

namespace SleepCover {

// Finds (generating it if needed) the cached cover BMP of the book in
// APP_STATE.openEpubPath: EPUB, XTC or TXT. False when no book is open or it
// has no cover; the reason is logged.
//
// The EPUB path lends the framebuffer to the metadata load as its inflate
// dictionary, so call this BEFORE drawing anything you want to keep.
bool resolveBmpPath(const GfxRenderer& renderer, bool cropped, std::string& outPath);

// The 8-bit grayscale twin of `coverPath` when it is the open EPUB's cached
// cover (generated if needed; JPEG/PNG covers only), for the Picco's 16-level
// sleep screen. False otherwise, or when it cannot be made. Like
// resolveBmpPath, it lends the framebuffer to the metadata load.
bool grayVersionOf(const GfxRenderer& renderer, const std::string& coverPath, std::string& grayPath);

}  // namespace SleepCover
