#pragma once

#include <string>

#include "components/themes/BaseTheme.h"

class Bitmap;
class GfxRenderer;
struct RecentBook;

// Shrunk-cover cache shared by the home themes that draw covers well below
// their thumbnail height (Bookshelf's shelf and hero, Licorice's row).
//
// Shrinking a 400 px thumbnail to a small slot reads the whole source and
// averages every pixel -- slow on the C3, which has no FPU, and paid for every
// book whenever the home strip is repainted. The shrunk, dithered result is a
// small 1-bit BMP, so it is cached next to the book's thumbnails and later
// drawn 1:1.
namespace CoverCache {

// Where and how a cover is drawn inside its box.
struct CoverPlacement {
  Rect box;
  bool bottomAlign;
  bool centerX;
  const char* cacheKey;  // one cached copy per use and box size, e.g. "shelf", "hero"
  int radius;
};

// Largest size inside the box at the source's own aspect, scaling up as well
// as down (drawBitmapResampled does both), so a small source still fills its
// slot instead of floating tiny in it. Placement is the caller's.
void fitSize(const Bitmap& bitmap, int boxW, int boxH, int& wOut, int& hOut);

// Draws the book's cover fitted into place.box at the artwork's aspect: from
// the cache when it is current, otherwise shrunk from the thumbnail and
// cached, then masked to rounded corners and outlined. False (nothing drawn)
// when there is no readable thumbnail.
bool drawCoverCached(const GfxRenderer& renderer, const RecentBook& book, const CoverPlacement& place, int thumbHeight,
                     Rect& drawnOut);

}  // namespace CoverCache
