#pragma once

#include <cstdint>

// The UC8253 X3's two-pass 16-level planes (freeink-sdk PR #220: GrayscaleCapabilities.h,
// namespace freeink::gray16), mirrored here so lib/ and the desktop simulator build without
// the SDK header. GfxRenderer.cpp static_asserts every value against the SDK on device builds.
//
// A page is five 1 bpp planes, each a render of the page where a pixel's 4-bit ink level
// (0 = white .. 15 = black) decides its bit through one of these masks:
//   base (DTM2 of the B/W turn)   kMaskBwClear:      ink (bit 0) where the mask has the level
//   pass 1 coarse  DTM1 / DTM2    kMaskPass1Dtm1/2:  bit 1 where the mask has the level
//   pass 2 fine    DTM1 / DTM2    kMaskPass2Dtm1/2:  bit 1 where the mask has the level
//   cleanup DTM1                  kMaskCleanupBlack: ink (bit 0) where the mask has the level
// GfxRenderer draws every plane in its ink-clears convention (white 0xFF, ink clears the bit,
// GRAY16_PLANE + setGray16LevelMask); the pass planes are then inverted whole, which turns
// "ink where the mask hits" into "1 where the mask hits" and the white background into 0.
// Levels 0 and 15 carry 0 in every pass plane: pure white and pure black get no drive.
namespace gray16planes {

inline constexpr uint16_t kMaskBwClear = 0xFF00u;       // base: levels 8..15 start black
inline constexpr uint16_t kMaskPass1Dtm1 = 0x24EEu;     // pass 1 coarse, DTM1 (the LSB plane upload)
inline constexpr uint16_t kMaskPass1Dtm2 = 0x3756u;     // pass 1 coarse, DTM2 (the MSB plane upload)
inline constexpr uint16_t kMaskPass2Dtm1 = 0x71DAu;     // pass 2 fine, DTM1
inline constexpr uint16_t kMaskPass2Dtm2 = 0x6D0Au;     // pass 2 fine, DTM2
inline constexpr uint16_t kMaskCleanupBlack = 0xFFFEu;  // cleanup DTM1: every inked level 0, white 1

// A 2-bit image or glyph value (0 = black, 1 = dark, 2 = light, 3 = white) on the four calibrated
// split-rail anchors (PR #220's 0xFF00 2-bit helpers: ink 15, 9, 6, 0).
inline constexpr uint8_t kImage2BitInk[4] = {15, 9, 6, 0};

// Bit-plane `plane` of the level: the levels whose bit `plane` is set. The Picco's four-plane
// fold (GfxRenderer::setGray16Plane) is this mask family.
constexpr uint16_t bitPlaneMask(const uint8_t plane) {
  uint16_t mask = 0;
  for (int level = 0; level < 16; ++level) {
    if ((level >> (plane & 3)) & 1) mask |= static_cast<uint16_t>(1u << level);
  }
  return mask;
}

// What the panel shows for a 5-bit plane tuple (bw, coarseLsb, coarseMsb, fineLsb, fineMsb), bw
// the base bit (1 = white), as a gray 0 = black .. 15 = white. For the simulator and tests: fed
// the planes above, level l decodes to gray 15 - l.
inline constexpr uint8_t kDecodeGray16[32] = {
    0, 4, 0, 1, 6, 0, 3, 7, 0, 0, 0, 0, 0, 5, 0, 2, 15, 15, 15, 15, 15, 15, 11, 15, 10, 15, 8, 12, 13, 15, 9, 14,
};

}  // namespace gray16planes
