#pragma once

#include <algorithm>
#include <cstdint>

// Glyph rasterizer that resolves orientation, clipping and framebuffer
// addressing once per glyph instead of once per pixel. It has no dependency
// on GfxRenderer so it can be unit-tested on the host.
namespace glyphBitmap {

// Position of glyph pixel (0, 0) plus the step for one move along the glyph's
// x and y axes, all in one coordinate space. The axes must describe an
// orthogonal unit rotation: each is +/-1 on exactly one component.
struct Frame {
  int x;
  int y;
  int dxX;
  int dxY;
  int dyX;
  int dyY;
};

// What each source pixel value does to the destination bit. Bit n of a mask
// refers to source value n (2bpp: 0=white, 1=light gray, 2=dark gray, 3=black;
// 1bpp: 1=ink). A value in neither mask leaves the destination untouched, so
// this covers both the sparse cases (B/W ink, overlay gray masks: one mask
// populated) and the absolute gray planes, where every value writes and some
// set while others clear. setBits and clearBits must be disjoint.
//
// Framebuffer polarity matches GfxRenderer::drawPixel(): clearing a bit is
// "black" (state == true), setting it is "white".
struct Levels {
  uint8_t setBits;
  uint8_t clearBits;
};

// Where a glyph lands in physical framebuffer space.
struct Target {
  uint8_t* buffer;  // First byte of row originY (framebuffer or strip scratch)
  int width;        // Physical panel width in pixels
  int stride;       // Bytes per physical row
  int originY;      // First physical row held by buffer
  int rows;         // Number of physical rows held by buffer
  Frame frame;      // Physical placement of the glyph
};

// Half-open rectangle in glyph-local pixel coordinates.
struct Clip {
  int left;
  int top;
  int right;
  int bottom;
};

// Narrow [start, end) on one glyph axis so that base + i * step stays within
// [lower, upper). step is +1 or -1 because the transform is orthogonal.
inline void clipAxis(int base, int step, int lower, int upper, int& start, int& end) {
  if (step > 0) {
    start = std::max(start, lower - base);
    end = std::min(end, upper - base);
  } else {
    start = std::max(start, base - upper + 1);
    end = std::min(end, base - lower + 1);
  }
}

// Narrow a glyph-local clip so every kept pixel maps inside the half-open
// rectangle [left, right) x [top, bottom) of the space frame is expressed in.
// Which glyph axis runs along that space's x depends on the rotation:
// dxX != 0 means glyph x does.
__attribute__((always_inline)) inline void clipToRect(const Frame& frame, int left, int top, int right, int bottom,
                                                      Clip& clip) {
  if (frame.dxX != 0) {
    clipAxis(frame.x, frame.dxX, left, right, clip.left, clip.right);
    clipAxis(frame.y, frame.dyY, top, bottom, clip.top, clip.bottom);
  } else {
    clipAxis(frame.x, frame.dyX, left, right, clip.top, clip.bottom);
    clipAxis(frame.y, frame.dxY, top, bottom, clip.left, clip.right);
  }
}

// Keep pixel writes inline when decoding a group of four pixels.
// destination is a flat bit index: (row * stride * 8 + physical x). stride * 8
// is a multiple of 8, so destination >> 3 is the byte and destination & 7 is
// physical x & 7 -- MSB first, the same bit drawPixel() addresses.
__attribute__((always_inline)) inline void paint(uint8_t* buffer, int destination, uint8_t value, uint8_t painted,
                                                 uint8_t clearBits) {
  const uint8_t bit = static_cast<uint8_t>(1u << value);
  if ((painted & bit) == 0) return;
  const uint8_t mask = static_cast<uint8_t>(0x80u >> (destination & 7));
  if (clearBits & bit)
    buffer[destination >> 3] &= static_cast<uint8_t>(~mask);
  else
    buffer[destination >> 3] |= mask;
}

// Paint a packed glyph into target.
//
// bitmap: rows are contiguous, MSB first, 1 or 2 bits per pixel; widths need
//   not be byte-aligned.
// levels: what each source value does, see Levels.
// Clipping happens before pixel decoding, so fully hidden glyphs cost nothing.
inline void draw(const uint8_t* bitmap, int width, int height, bool twoBit, Levels levels, const Target& target,
                 Clip clip) {
  const uint8_t painted = levels.setBits | levels.clearBits;
  const uint8_t clearBits = levels.clearBits;
  if (painted == 0) return;

  // Intersect with the glyph bounds, then with the panel width and the
  // buffered row band.
  clip.left = std::max(clip.left, 0);
  clip.top = std::max(clip.top, 0);
  clip.right = std::min(clip.right, width);
  clip.bottom = std::min(clip.bottom, height);
  clipToRect(target.frame, 0, target.originY, target.width, target.originY + target.rows, clip);
  if (clip.left >= clip.right || clip.top >= clip.bottom) return;

  // Walk the framebuffer as a flat bit index. One glyph column advances
  // stepX bits and one glyph row advances stepY bits; each is +/-1 for the
  // axis that maps to physical x, or +/-strideBits for physical y.
  const Frame& frame = target.frame;
  const int strideBits = target.stride * 8;
  const int stepX = frame.dxY * strideBits + frame.dxX;
  const int stepY = frame.dyY * strideBits + frame.dyX;
  int rowBit = (frame.y - target.originY) * strideBits + frame.x + clip.left * stepX + clip.top * stepY;
  for (int y = clip.top; y < clip.bottom; ++y, rowBit += stepY) {
    int source = y * width + clip.left;
    int destination = rowBit;
    int remaining = clip.right - clip.left;
    if (twoBit) {
      // Rows are bit-contiguous, not byte-padded. Align after any clipped prefix.
      while (remaining && (source & 3)) {
        paint(target.buffer, destination, (bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3, painted, clearBits);
        ++source;
        destination += stepX;
        --remaining;
      }
      while (remaining >= 4) {
        const uint8_t packed = bitmap[source >> 2];
        // An all-white byte is the common case inside a glyph's bounding box;
        // skip it in one test when white is not a painted value.
        if (packed != 0 || (painted & 0x01)) {
          paint(target.buffer, destination, packed >> 6, painted, clearBits);
          paint(target.buffer, destination + stepX, (packed >> 4) & 3, painted, clearBits);
          paint(target.buffer, destination + 2 * stepX, (packed >> 2) & 3, painted, clearBits);
          paint(target.buffer, destination + 3 * stepX, packed & 3, painted, clearBits);
        }
        source += 4;
        destination += 4 * stepX;
        remaining -= 4;
      }
    }
    // One-bit glyphs and the clipped tail use scalar decoding.
    while (remaining--) {
      const uint8_t value = twoBit ? ((bitmap[source >> 2] >> (6 - (source & 3) * 2)) & 3)
                                   : ((bitmap[source >> 3] >> (7 - (source & 7))) & 1);
      paint(target.buffer, destination, value, painted, clearBits);
      ++source;
      destination += stepX;
    }
  }
}

}  // namespace glyphBitmap
