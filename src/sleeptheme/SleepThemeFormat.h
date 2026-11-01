#pragma once

#include <cstddef>
#include <cstdint>

// On-disk layout of a compiled sleep theme (theme.bin), written by the
// browser compiler in src/network/html/SleepThemesPage.html and documented in
// docs/file-formats.md. All fields little-endian. Records are fixed-size so
// the loader reads them with memcpy into aligned structs (RISC-V faults on
// unaligned multi-byte loads).
//
// A theme folder (/sleep-themes/<name>/) holds:
//   theme.bin   this layout
//   bg.bmp      1-bit background, exactly screenWidth x screenHeight
//   f0..f3.cpfont  fonts for the live text, referenced by fontIndex
namespace SleepThemeFormat {

inline constexpr char MAGIC[8] = {'K', 'P', 'T', 'H', 'E', 'M', 'E', '\0'};
inline constexpr uint16_t VERSION = 1;
inline constexpr size_t HEADER_SIZE = 32;
inline constexpr size_t ELEMENT_SIZE = 48;
inline constexpr uint8_t MAX_ELEMENTS = 64;
inline constexpr uint8_t MAX_FONTS = 4;
inline constexpr uint16_t MAX_STRING_TABLE = 4096;
inline constexpr uint16_t NO_STRING = 0xFFFF;

enum class ElementKind : uint8_t {
  Text = 0,   // template string drawn in a theme font
  Cover = 1,  // the open book's cover, resampled into the box
  Bar = 2,    // box filled black from the left by a 0-100 value
};

enum class TextAlign : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class TextTransform : uint8_t { None = 0, Upper = 1, Lower = 2, Capitalize = 3 };

// Header flags
inline constexpr uint8_t HEADER_FLAG_COVER_TEMPLATES = 0x01;  // cover elements store their value's template

// Element flags
inline constexpr uint8_t FLAG_WHITE = 0x01;          // draw ink white instead of black
inline constexpr uint8_t FLAG_HIDE_IF_EMPTY = 0x02;  // (cover/bar) skip when the value is empty
inline constexpr uint8_t FLAG_TRANSFORMED = 0x04;    // matrix is not a pure translation
inline constexpr uint8_t FLAG_ELLIPSIS = 0x08;       // truncate overflowing text with an ellipsis
inline constexpr uint8_t FLAG_CONTAIN = 0x10;        // (cover) keep aspect ratio inside the box
inline constexpr uint8_t FLAG_OPAQUE = 0x20;         // (cover) paint white under the image

struct Header {
  char magic[8];
  uint16_t version;
  uint16_t screenWidth;  // logical width the theme was laid out at
  uint16_t screenHeight;
  uint8_t orientation;  // GfxRenderer::Orientation
  uint8_t elementCount;
  uint8_t fontCount;
  uint8_t flags;  // HEADER_FLAG_*
  uint16_t stringTableSize;
  uint8_t reserved[12];
};
static_assert(sizeof(Header) == HEADER_SIZE, "theme.bin header size");

// One live element. The box is (0,0)-(width,height) in the element's own
// coordinates; matrix maps those to screen coordinates, CSS/DOMMatrix order:
//   screenX = a*x + c*y + e,  screenY = b*x + d*y + f
// in 16.16 fixed point (the C3 has no FPU).
struct Element {
  uint8_t kind;  // ElementKind
  uint8_t fontIndex;
  uint8_t fontStyle;  // EpdFontFamily::Style (0 regular .. 3 bold-italic)
  uint8_t align;      // TextAlign
  uint8_t flags;
  uint8_t textTransform;  // TextTransform
  uint8_t maxLines;       // 0 or 1 = single line
  int8_t digitIndex;      // >= 0: draw only the nth digit from the right of the value
  int8_t tracking;        // letter-spacing in half-pixel steps (GfxRenderer tracking)
  uint8_t reserved0;
  uint16_t width;
  uint16_t height;
  uint16_t templateOffset;  // into the string table, NO_STRING for none
  int32_t matrix[6];        // a, b, c, d, e, f
  uint16_t lineHeight;      // pixels between baselines, 0 = font default
  uint16_t barMax;          // (bar) value that fills the bar; 0 = 100, a percentage
  uint8_t reserved[4];
};
static_assert(sizeof(Element) == ELEMENT_SIZE, "theme.bin element size");

}  // namespace SleepThemeFormat
