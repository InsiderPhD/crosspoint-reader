#pragma once

#include <cstdint>

// Per-cover shadow lift for the cover/thumbnail converters.
//
// Why: on the home and library screens a 2-bit cover is drawn with only its
// brightest level as white (GfxRenderer::drawBitmap, BW mode: val < 3 is
// black), and the 2-bit quantizer puts that level at source luminance >= 140.
// A dark cover whose title sits at 80-130 therefore collapses into a black
// block. Stretching such a cover so its bright content reaches white makes it
// readable again; a cover that is already light must not move at all.
//
// How: the converters run a cheap pre-pass over the source (JPEG: 1/8-scale
// decode, PNG: a first inflate pass) into a LumaHistogram, bake the curve into
// a 256-entry table, and apply the table per pixel ahead of the ditherer. The
// curve is a plain contrast stretch — black point at the 2nd percentile, gain
// chosen so the 90th percentile lands at kStretchTarget — faded in by how dark
// the cover is overall, so a mid-tone cover gets a mild lift and a light cover
// an identity table. No float: the table is built with integer arithmetic.
//
// Host-testable: no Arduino or HAL dependencies (see test/cover_tone_curve).

class LumaHistogram {
 public:
  static constexpr int kBins = 32;  // 8 luminance levels per bin
  static constexpr int kLevelsPerBin = 256 / kBins;

  LumaHistogram() { reset(); }

  void reset();
  void add(const uint8_t gray) {
    static_assert(kLevelsPerBin == 8, "bin shift assumes 8 levels per bin");
    bins[gray >> 3]++;
  }
  // Hot path of the pre-pass: one increment per source pixel.
  void addRow(const uint8_t* row, int count);

  uint32_t total() const;
  // Mean luminance 0-255 (bin centres); 0 for an empty histogram.
  uint8_t mean() const;
  // Index of the bin holding the pct-th percentile pixel (0..kBins-1).
  int percentileBin(int pct) const;

 private:
  uint32_t bins[kBins];
};

class CoverToneCurve {
 public:
  // Tuning. Luminance is 0-255 source gray; gains are in 1/256ths.
  static constexpr int kMeanFullLift = 90;      // mean at or below this: full-strength stretch
  static constexpr int kMeanNoLift = 150;       // mean at or above this: identity (light covers)
  static constexpr int kStretchTarget = 230;    // where the bright percentile is moved to
  static constexpr int kBrightPercentile = 90;  // "the cover's bright content"
  static constexpr int kDarkPercentile = 2;     // black point
  static constexpr int kMaxGain256 = 3 * 256;   // cap for near-black covers

  CoverToneCurve() { setIdentity(); }

  void setIdentity();
  // Builds the curve for one cover. An empty histogram yields the identity.
  void build(const LumaHistogram& hist);

  uint8_t apply(const uint8_t gray) const { return lut[gray]; }
  const uint8_t* table() const { return lut; }
  bool isIdentity() const { return identity; }

  // For logging: the effective gain (1/256ths) and black point that built the table.
  uint16_t gain256() const { return effectiveGain256; }
  uint8_t blackPoint() const { return effectiveBlackPoint; }

 private:
  uint8_t lut[256];
  bool identity = true;
  uint16_t effectiveGain256 = 256;
  uint8_t effectiveBlackPoint = 0;
};

// The converters run strictly one at a time (they already share static file
// state), so one 256-byte table serves both of them instead of each carrying
// its own. Returns the shared instance.
CoverToneCurve& sharedCoverToneCurve();
