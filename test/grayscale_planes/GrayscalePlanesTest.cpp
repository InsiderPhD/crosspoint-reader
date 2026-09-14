// Encoding of a 2-bit grey level into the two grayscale planes, for both
// schemes the panels accept. The absolute scheme is what lets a controller
// drive a page without recovering the base from whatever ink is already on the
// panel; getting a single level wrong there shows up as a whole page of wrong
// tones, so the table is pinned here rather than trusted to review.
//
// Reference (freeink-sdk GrayscaleCapabilities.h), in (LSB, MSB) bit order:
//   overlay masks   black/white = 00 (deferred to the B/W base), dark = 11, light = 01
//   absolute planes black = 00, dark = 10, light = 01, white = 11

#include <cstdio>

#include "lib/GfxRenderer/BitmapHelpers.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                                            \
  do {                                                                                             \
    if ((a) != (b)) {                                                                              \
      fprintf(stderr, "  FAIL: %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, (a), (b)); \
      testsFailed++;                                                                               \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

#define PASS() testsPassed++

namespace {

constexpr uint8_t kBlack = 0, kDark = 1, kLight = 2, kWhite = 3;

// Resolve one level to the bit that ends up in a plane, mirroring what
// drawPixel() does: state true clears the bit, state false sets it. Overlay
// scratch is cleared to 0x00, absolute scratch to 0xFF.
int planeBit(const uint8_t level, const bool msb, const bool absolute) {
  const auto pixel = grayPlanePixel(level, msb, absolute);
  if (!pixel.write) return absolute ? 1 : 0;  // untouched: the cleared value
  return pixel.black ? 0 : 1;
}

// Two-bit (LSB, MSB) code for a level, as the controller sees it.
int planeCode(const uint8_t level, const bool absolute) {
  return planeBit(level, false, absolute) * 10 + planeBit(level, true, absolute);
}

void testAbsolutePlanesCarryEveryLevel() {
  ASSERT_EQ(planeCode(kBlack, true), 0);
  ASSERT_EQ(planeCode(kDark, true), 10);
  ASSERT_EQ(planeCode(kLight, true), 1);
  ASSERT_EQ(planeCode(kWhite, true), 11);
  PASS();
}

void testAbsolutePlanesWriteEveryPixel() {
  // Nothing may be left to the B/W base: an unwritten pixel would show as the
  // cleared background instead of its real tone.
  for (uint8_t level = 0; level <= 3; level++) {
    ASSERT_EQ(grayPlanePixel(level, false, true).write, true);
    ASSERT_EQ(grayPlanePixel(level, true, true).write, true);
  }
  PASS();
}

void testOverlayMasksDeferBlackAndWhite() {
  ASSERT_EQ(planeCode(kBlack, false), 0);
  ASSERT_EQ(planeCode(kDark, false), 11);
  ASSERT_EQ(planeCode(kLight, false), 1);
  ASSERT_EQ(planeCode(kWhite, false), 0);
  PASS();
}

void testOverlayMasksTouchOnlyIntermediateLevels() {
  // The two extremes must stay untouched so the separately displayed B/W frame
  // decides them.
  ASSERT_EQ(grayPlanePixel(kBlack, false, false).write, false);
  ASSERT_EQ(grayPlanePixel(kBlack, true, false).write, false);
  ASSERT_EQ(grayPlanePixel(kWhite, false, false).write, false);
  ASSERT_EQ(grayPlanePixel(kWhite, true, false).write, false);
  PASS();
}

void testOverlayMatchesTheLadderItReplaced() {
  // The hand-written ladder this helper replaced, kept verbatim: a regression
  // here silently changes every anti-aliased page on every device.
  for (uint8_t level = 0; level <= 3; level++) {
    ASSERT_EQ(grayPlanePixel(level, true, false).write, (level == 1 || level == 2));
    ASSERT_EQ(grayPlanePixel(level, false, false).write, (level == 1));
    // The overlay scheme only ever sets bits; it never clears one.
    ASSERT_EQ(grayPlanePixel(level, true, false).black, false);
    ASSERT_EQ(grayPlanePixel(level, false, false).black, false);
  }
  PASS();
}

void testEncodingsAreDistinguishable() {
  // Every level must be a distinct code within a scheme, or two tones collapse.
  const int absolute[4] = {planeCode(0, true), planeCode(1, true), planeCode(2, true), planeCode(3, true)};
  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 4; j++) {
      ASSERT_EQ(absolute[i] == absolute[j], false);
    }
  }
  PASS();
}

}  // namespace

int main() {
  printf("Grayscale plane encoding\n");
  testAbsolutePlanesCarryEveryLevel();
  testAbsolutePlanesWriteEveryPixel();
  testOverlayMasksDeferBlackAndWhite();
  testOverlayMasksTouchOnlyIntermediateLevels();
  testOverlayMatchesTheLadderItReplaced();
  testEncodingsAreDistinguishable();

  printf("  %d passed, %d failed\n", testsPassed, testsFailed);
  return testsFailed == 0 ? 0 : 1;
}
