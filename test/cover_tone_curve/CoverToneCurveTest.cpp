// The adaptive cover lift (lib/GfxRenderer/CoverToneCurve) decides, per cover,
// whether and how far to stretch luminance before dithering. Getting it wrong
// in one direction leaves dark covers as black blocks on the home screen; in
// the other it brightens covers that were fine. Both halves are pinned here.
//
// Reference for the "readable" bar: on the BW home/library screens only the
// top 2-bit level is white, and the quantizer puts that at source gray >= 140.

#include <cstdio>

#include "lib/GfxRenderer/CoverToneCurve.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define ASSERT_EQ(a, b)                                                                                      \
  do {                                                                                                       \
    if ((a) != (b)) {                                                                                        \
      fprintf(stderr, "  FAIL: %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, (int)(a), (int)(b)); \
      testsFailed++;                                                                                         \
      return;                                                                                                \
    }                                                                                                        \
  } while (0)

#define PASS() testsPassed++

namespace {

constexpr int kWhiteOnBw = 140;  // AtkinsonDitherer level-3 threshold

// Fill `count` pixels at luminance `gray`.
void fill(LumaHistogram& h, const uint8_t gray, const int count) {
  for (int i = 0; i < count; i++) h.add(gray);
}

bool isIdentityTable(const CoverToneCurve& c) {
  for (int i = 0; i < 256; i++)
    if (c.apply(static_cast<uint8_t>(i)) != i) return false;
  return true;
}

bool isMonotone(const CoverToneCurve& c) {
  for (int i = 1; i < 256; i++)
    if (c.apply(static_cast<uint8_t>(i)) < c.apply(static_cast<uint8_t>(i - 1))) return false;
  return true;
}

void testEmptyHistogramIsIdentity() {
  LumaHistogram h;
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(c.isIdentity());
  ASSERT_TRUE(isIdentityTable(c));
  PASS();
}

// White background, black text: the classic light cover. Must not move.
void testLightCoverUntouched() {
  LumaHistogram h;
  fill(h, 245, 9000);
  fill(h, 10, 1000);
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(c.isIdentity());
  ASSERT_TRUE(isIdentityTable(c));
  PASS();
}

// Pale cream cover with grey text (everything light, nothing at 255).
void testPaleCoverUntouched() {
  LumaHistogram h;
  fill(h, 215, 8000);
  fill(h, 120, 2000);
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(c.isIdentity());
  PASS();
}

// Dark background with a mid-grey title: the complaint. The title must clear
// the BW white threshold; the background must stay well below it.
void testDarkCoverTitleBecomesReadable() {
  LumaHistogram h;
  fill(h, 20, 8500);
  fill(h, 100, 1500);
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(!c.isIdentity());
  ASSERT_TRUE(c.apply(100) >= kWhiteOnBw);
  ASSERT_TRUE(c.apply(20) < 64);
  ASSERT_TRUE(isMonotone(c));
  ASSERT_EQ(c.apply(0), 0);
  ASSERT_EQ(c.apply(255), 255);
  PASS();
}

// Dark background with an already-white title is readable as it is: the
// bright percentile sits at the top, so there is nothing to stretch.
void testDarkCoverWhiteTitleUntouched() {
  LumaHistogram h;
  fill(h, 20, 8500);
  fill(h, 250, 1500);
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(c.isIdentity());
  PASS();
}

// Near-black cover: the gain is capped, so it brightens "a little" rather than
// turning sensor noise into a white field.
void testNearBlackCoverCapped() {
  LumaHistogram h;
  fill(h, 5, 6000);
  fill(h, 30, 4000);
  CoverToneCurve c;
  c.build(h);
  ASSERT_TRUE(!c.isIdentity());
  ASSERT_EQ(c.gain256(), CoverToneCurve::kMaxGain256);
  ASSERT_TRUE(c.apply(30) < kWhiteOnBw);
  ASSERT_TRUE(c.apply(30) > 30);
  PASS();
}

// Mid-tone photographic cover: a partial lift, strictly weaker than the full one.
void testMidToneCoverGetsPartialLift() {
  LumaHistogram full, mid;
  fill(full, 30, 7000);
  fill(full, 150, 3000);  // mean ~66 -> full strength
  fill(mid, 90, 7000);
  fill(mid, 180, 3000);  // mean ~117 -> partial strength
  CoverToneCurve cf, cm;
  cf.build(full);
  cm.build(mid);
  ASSERT_TRUE(!cf.isIdentity());
  ASSERT_TRUE(!cm.isIdentity());
  ASSERT_TRUE(cm.gain256() > 256);
  ASSERT_TRUE(cm.gain256() < cf.gain256());
  ASSERT_TRUE(isMonotone(cm));
  PASS();
}

// A rebuilt curve must forget the previous cover entirely.
void testRebuildResetsToIdentity() {
  LumaHistogram dark, light;
  fill(dark, 20, 9000);
  fill(dark, 100, 1000);
  fill(light, 240, 10000);
  CoverToneCurve c;
  c.build(dark);
  ASSERT_TRUE(!c.isIdentity());
  c.build(light);
  ASSERT_TRUE(c.isIdentity());
  ASSERT_TRUE(isIdentityTable(c));
  PASS();
}

void testHistogramStats() {
  LumaHistogram h;
  fill(h, 0, 50);
  fill(h, 255, 50);
  ASSERT_EQ(h.total(), 100u);
  ASSERT_EQ(h.percentileBin(2), 0);
  ASSERT_EQ(h.percentileBin(50), 0);
  ASSERT_EQ(h.percentileBin(51), LumaHistogram::kBins - 1);
  ASSERT_EQ(h.percentileBin(90), LumaHistogram::kBins - 1);
  // Bin centres: (4 + 252) / 2
  ASSERT_EQ(h.mean(), 128);
  PASS();
}

}  // namespace

int main() {
  testEmptyHistogramIsIdentity();
  testLightCoverUntouched();
  testPaleCoverUntouched();
  testDarkCoverTitleBecomesReadable();
  testDarkCoverWhiteTitleUntouched();
  testNearBlackCoverCapped();
  testMidToneCoverGetsPartialLift();
  testRebuildResetsToIdentity();
  testHistogramStats();

  printf("%d passed, %d failed\n", testsPassed, testsFailed);
  return testsFailed == 0 ? 0 : 1;
}
