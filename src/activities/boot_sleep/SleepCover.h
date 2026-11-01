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

}  // namespace SleepCover
