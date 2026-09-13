#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra Carousel theme metrics (zero runtime cost).
//
// The strip is tall because the centre cover is nearly full width: 590 px from
// homeTopPadding leaves the title block, the cover, the dot row and the
// progress footer inside rect, with the icon menu anchored separately to the
// screen bottom. See the layout arithmetic in LyraCarouselTheme.cpp.
namespace LyraCarouselMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  v.homeCoverHeight = 600;      // thumbnail cache key; bounds artwork to 360x600
  v.homeCoverTileHeight = 590;  // drawn strip height
  v.homeRecentBooksCount = 3;
  return v;
}();

#if FREEINK_DEVICE_X4PRO
// Gesture mode (Full Touch off) reclaims the action-bar strip; see BaseMetrics.
constexpr ThemeMetrics noActionBarValues = [] {
  ThemeMetrics v = values;
  v.buttonHintsHeight = 0;
  return v;
}();
#endif
}  // namespace LyraCarouselMetrics

// Coverflow home screen: the selected book fills the centre of the strip with
// the neighbouring covers angled away to either side.
//
// Deviations from the CrossInk theme this was ported from, all deliberate:
//  - The centre cover is letterboxed at the artwork's true aspect and never
//    scaled past its intrinsic pixels, rather than cropped to fill a fixed
//    rect. Side covers keep their nominal trapezoid geometry: the horizontal
//    squeeze there is the perspective effect, not an aspect error.
//  - No pre-rendered framebuffer cache. CrossInk keeps whole 48 KB frames in
//    RAM and spills them to SD; that does not fit this fork's heap budget.
//  - The selected menu icon sits on a LightGray plate instead of a black one,
//    because GfxRenderer::drawIcon is transparent-only and cannot draw an
//    icon knocked out in white.
class LyraCarouselTheme : public LyraTheme {
 public:
  // Nominal artwork geometry. Boxes, not output sizes — drawCenterCover fits
  // the real bitmap inside kCenterBox* at its own aspect.
  static constexpr int kCenterBoxW = 316;
  static constexpr int kCenterBoxH = 488;
  static constexpr int kNearSideW = 75;
  static constexpr int kFarSideW = 61;
  static constexpr int kNearSideInnerH = 417;
  static constexpr int kNearSideOuterH = 380;
  static constexpr int kFarSideInnerH = 389;
  static constexpr int kFarSideOuterH = 343;

  const ThemeMetrics& themeMetrics() const override;

  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;

  // The carousel's cover slots overlap and the centre one is far wider than a
  // third of the strip, so BaseTheme's equal-column split resolves taps to the
  // wrong book. Hit-tests the drawn geometry instead.
  int hitTestRecentBookCover(const Rect& rect, int slotCount, int lx, int ly) const override;

  // The menu is a horizontal row anchored to the screen bottom, not the stack
  // of full-width rows BaseTheme assumes, so the default hit test resolves
  // every tap in the row to one of its first two tiles.
  int hitTestButtonMenu(const Rect& rect, int buttonCount, int lx, int ly) const override;
};
