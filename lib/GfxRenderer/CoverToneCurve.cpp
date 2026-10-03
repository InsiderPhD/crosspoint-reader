#include "CoverToneCurve.h"

#include <cstring>

void LumaHistogram::reset() { memset(bins, 0, sizeof(bins)); }

void LumaHistogram::addRow(const uint8_t* row, const int count) {
  for (int i = 0; i < count; i++) bins[row[i] >> 3]++;
}

uint32_t LumaHistogram::total() const {
  uint32_t n = 0;
  for (const uint32_t b : bins) n += b;
  return n;
}

uint8_t LumaHistogram::mean() const {
  const uint32_t n = total();
  if (n == 0) return 0;
  // Bin centre (index * 8 + 4) weighted by count; 64-bit so a 2048x3072 source
  // (6.3M pixels x 255) cannot overflow.
  uint64_t sum = 0;
  for (int i = 0; i < kBins; i++) sum += static_cast<uint64_t>(bins[i]) * (i * kLevelsPerBin + kLevelsPerBin / 2);
  return static_cast<uint8_t>(sum / n);
}

int LumaHistogram::percentileBin(const int pct) const {
  const uint32_t n = total();
  if (n == 0) return 0;
  // Smallest bin whose cumulative count reaches pct% of the pixels.
  const uint64_t target = (static_cast<uint64_t>(n) * pct + 99) / 100;
  uint64_t cumulative = 0;
  for (int i = 0; i < kBins; i++) {
    cumulative += bins[i];
    if (cumulative >= target) return i;
  }
  return kBins - 1;
}

void CoverToneCurve::setIdentity() {
  for (int i = 0; i < 256; i++) lut[i] = static_cast<uint8_t>(i);
  identity = true;
  effectiveGain256 = 256;
  effectiveBlackPoint = 0;
}

void CoverToneCurve::build(const LumaHistogram& hist) {
  setIdentity();
  if (hist.total() == 0) return;

  // Strength 0..256 from the mean: light covers are left alone, dark covers get
  // the whole stretch, mid-tone covers a proportional share.
  const int mean = hist.mean();
  int strength256 = 0;
  if (mean <= kMeanFullLift) {
    strength256 = 256;
  } else if (mean < kMeanNoLift) {
    strength256 = ((kMeanNoLift - mean) * 256) / (kMeanNoLift - kMeanFullLift);
  }
  if (strength256 == 0) return;

  // Black point: lower edge of the dark-percentile bin (conservative, so real
  // shadow detail just above it survives). Bright point: upper edge of the
  // bright-percentile bin.
  const int blackPoint = hist.percentileBin(kDarkPercentile) * LumaHistogram::kLevelsPerBin;
  const int brightPoint =
      hist.percentileBin(kBrightPercentile) * LumaHistogram::kLevelsPerBin + (LumaHistogram::kLevelsPerBin - 1);

  // Gain that carries brightPoint to kStretchTarget after the black point is
  // removed. A cover whose bright content is already at or past the target
  // (gain <= 1) is left as it is; near-black covers hit the cap.
  const int span = brightPoint - blackPoint;
  int gain256 = (span > 0) ? (kStretchTarget * 256) / span : kMaxGain256;
  if (gain256 > kMaxGain256) gain256 = kMaxGain256;
  if (gain256 <= 256) return;

  // Fade both parameters in by strength.
  const int gainEff256 = 256 + (((gain256 - 256) * strength256) >> 8);
  const int blackEff = (blackPoint * strength256) >> 8;

  for (int i = 0; i < 256; i++) {
    int v = i - blackEff;
    if (v < 0) v = 0;
    v = (v * gainEff256 + 128) >> 8;
    if (v > 255) v = 255;
    lut[i] = static_cast<uint8_t>(v);
  }
  identity = false;
  effectiveGain256 = static_cast<uint16_t>(gainEff256);
  effectiveBlackPoint = static_cast<uint8_t>(blackEff);
}

CoverToneCurve& sharedCoverToneCurve() {
  static CoverToneCurve curve;
  return curve;
}
