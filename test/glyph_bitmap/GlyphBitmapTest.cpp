// Host test for glyphBitmap::draw (lib/GfxRenderer/GlyphBitmap.h): the 1/2-bit
// paths every board uses, and the 4-bit path the Picco's 16-level text uses
// (one 1 bpp plane per level bit, folded back into levels).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "GlyphBitmap.h"

using glyphBitmap::Clip;
using glyphBitmap::Frame;
using glyphBitmap::Levels;
using glyphBitmap::Target;

namespace {
constexpr int W = 16, H = 4, STRIDE = W / 8;

bool ink(const std::vector<uint8_t>& fb, int x, int y) { return !((fb[y * STRIDE + x / 8] >> (7 - x % 8)) & 1); }

std::vector<uint8_t> drawGlyph(const uint8_t* bitmap, int w, int h, uint8_t bits, Levels levels, Clip clip) {
  std::vector<uint8_t> fb(STRIDE * H, 0xFF);  // white
  const Target t{fb.data(), W, STRIDE, 0, H, Frame{1, 0, 1, 0, 0, 1}};
  glyphBitmap::draw(bitmap, w, h, bits, levels, t, clip);
  return fb;
}

// Pack 4-bit values, rows bit-contiguous, high nibble first.
std::vector<uint8_t> pack4(const std::vector<uint8_t>& v) {
  std::vector<uint8_t> out((v.size() + 1) / 2, 0);
  for (size_t i = 0; i < v.size(); ++i) out[i / 2] |= static_cast<uint8_t>(v[i] << ((i & 1) ? 0 : 4));
  return out;
}
}  // namespace

int main() {
  // 2-bit: values 0..3 in one row; BW-style masks ink every non-white value.
  {
    const uint8_t glyph[] = {0b00011011};  // 0, 1, 2, 3
    const auto fb = drawGlyph(glyph, 4, 1, 2, Levels{0, 0b1110}, Clip{0, 0, 4, 1});
    assert(!ink(fb, 1, 0) && ink(fb, 2, 0) && ink(fb, 3, 0) && ink(fb, 4, 0));
    // Overlay-style: only value 1 paints, white (set) instead of black.
    std::vector<uint8_t> pre(STRIDE * H, 0x00);
    const Target t{pre.data(), W, STRIDE, 0, H, Frame{1, 0, 1, 0, 0, 1}};
    glyphBitmap::draw(glyph, 4, 1, 2, Levels{0b0010, 0}, t, Clip{0, 0, 4, 1});
    assert(!ink(pre, 2, 0) && ink(pre, 1, 0) && ink(pre, 3, 0));
  }
  // 1-bit unchanged.
  {
    const uint8_t glyph[] = {0b10100000};
    const auto fb = drawGlyph(glyph, 3, 1, 1, Levels{0, 0b10}, Clip{0, 0, 3, 1});
    assert(ink(fb, 1, 0) && !ink(fb, 2, 0) && ink(fb, 3, 0));
  }
  // 4-bit decode, odd width so rows run through a byte: 5 x 3.
  std::vector<uint8_t> values;
  for (int i = 0; i < 15; ++i) values.push_back(static_cast<uint8_t>(i + 1));  // 1..15
  const auto glyph4 = pack4(values);
  for (int i = 0; i < 15; ++i) assert(glyphBitmap::valueAt(glyph4.data(), i, 4) == values[i]);
  // Planes: value v inks in plane p iff bit p of v; the four planes rebuild v.
  int rebuilt[15] = {};
  for (int plane = 0; plane < 4; ++plane) {
    Levels lv{0, 0};
    for (int v = 1; v < 16; ++v)
      if ((v >> plane) & 1) lv.clearBits |= static_cast<uint16_t>(1u << v);
    const auto fb = drawGlyph(glyph4.data(), 5, 3, 4, lv, Clip{0, 0, 5, 3});
    for (int i = 0; i < 15; ++i)
      if (ink(fb, 1 + i % 5, i / 5)) rebuilt[i] |= 1 << plane;
  }
  for (int i = 0; i < 15; ++i) assert(rebuilt[i] == values[i]);
  // Clipping on the 4-bit path: only glyph columns 1..2 of rows 0..1.
  {
    Levels all{0, 0xFFFE};
    const auto fb = drawGlyph(glyph4.data(), 5, 3, 4, all, Clip{1, 0, 3, 2});
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) assert(ink(fb, x, y) == (y < 2 && x >= 2 && x <= 3));
  }
  // 8-bit: one byte per pixel; draw8 follows the per-value action table and
  // clips like draw(). 4 x 2 glyph, values 0, 64, 128, 255 per row.
  {
    const uint8_t glyph8[] = {0, 64, 128, 255, 255, 128, 64, 0};
    for (int i = 0; i < 8; ++i) assert(glyphBitmap::valueAt(glyph8, i, 8) == glyph8[i]);
    uint8_t action[256] = {};
    for (int v = 128; v < 256; ++v) action[v] = 2;  // ink from half coverage
    action[64] = 1;                                 // 64 sets (white) on a black page
    std::vector<uint8_t> fb(STRIDE * H, 0xFF);
    const Target t{fb.data(), W, STRIDE, 0, H, Frame{1, 0, 1, 0, 0, 1}};
    glyphBitmap::draw8(glyph8, 4, 2, action, t, Clip{0, 0, 4, 2});
    assert(!ink(fb, 1, 0) && !ink(fb, 2, 0) && ink(fb, 3, 0) && ink(fb, 4, 0));
    assert(ink(fb, 1, 1) && ink(fb, 2, 1) && !ink(fb, 3, 1) && !ink(fb, 4, 1));
    std::vector<uint8_t> black(STRIDE * H, 0x00);
    const Target tb{black.data(), W, STRIDE, 0, H, Frame{1, 0, 1, 0, 0, 1}};
    glyphBitmap::draw8(glyph8, 4, 2, action, tb, Clip{0, 0, 4, 2});
    assert(!ink(black, 2, 0) && ink(black, 1, 0));  // 64 -> white, 0 untouched
    // Clipped to glyph column 3 of row 0 only.
    std::vector<uint8_t> clipped(STRIDE * H, 0xFF);
    const Target tc{clipped.data(), W, STRIDE, 0, H, Frame{1, 0, 1, 0, 0, 1}};
    glyphBitmap::draw8(glyph8, 4, 2, action, tc, Clip{3, 0, 4, 1});
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) assert(ink(clipped, x, y) == (y == 0 && x == 4));
  }
  std::printf("GlyphBitmapTest: 1/2-bit paths, 4/8-bit decode, plane fold and clipping passed\n");
  return 0;
}
