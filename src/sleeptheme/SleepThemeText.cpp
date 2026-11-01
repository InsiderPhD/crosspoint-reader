#include "SleepThemeText.h"

#include <Utf8.h>

#include <cstdint>
#include <cstdio>

namespace SleepThemeText {

namespace {

using SleepThemeFormat::TextTransform;

uint32_t toUpper(const uint32_t cp) {
  if (cp >= 'a' && cp <= 'z') return cp - 0x20;
  if (cp >= 0x00E0 && cp <= 0x00FE && cp != 0x00F7) return cp - 0x20;  // Latin-1, not ÷
  if (cp == 0x00FF) return 0x0178;                                     // ÿ
  // Latin Extended-A: alternating pairs, uppercase first, in two runs that
  // flip parity around the ı/ĸ singletons.
  if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) || (cp >= 0x014A && cp <= 0x0177)) {
    return (cp & 1) ? cp - 1 : cp;
  }
  if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E)) return (cp & 1) ? cp : cp - 1;
  if (cp == 0x03C2) return 0x03A3;                     // final sigma
  if (cp >= 0x03B1 && cp <= 0x03C9) return cp - 0x20;  // Greek
  if (cp >= 0x0430 && cp <= 0x044F) return cp - 0x20;  // Cyrillic basic
  if (cp >= 0x0450 && cp <= 0x045F) return cp - 0x50;  // Cyrillic Ѐ-Џ
  return cp;
}

uint32_t toLower(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
  if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) return cp + 0x20;  // Latin-1, not ×
  if (cp == 0x0178) return 0x00FF;
  if ((cp >= 0x0100 && cp <= 0x012F) || (cp >= 0x0132 && cp <= 0x0137) || (cp >= 0x014A && cp <= 0x0177)) {
    return (cp & 1) ? cp : cp + 1;
  }
  if ((cp >= 0x0139 && cp <= 0x0148) || (cp >= 0x0179 && cp <= 0x017E)) return (cp & 1) ? cp + 1 : cp;
  if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;
  return cp;
}

// A word starts after anything that is not a letter, digit or apostrophe.
bool isWordChar(const uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '\'' ||
         cp >= 0x00C0;  // includes U+2019, the typographic apostrophe
}

size_t encode(const uint32_t cp, char* out, const size_t room) {
  if (cp < 0x80) {
    if (room < 1) return 0;
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    if (room < 2) return 0;
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    if (room < 3) return 0;
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (room < 4) return 0;
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

}  // namespace

void applyTransform(const TextTransform transform, const char* in, char* out, const size_t outSize) {
  if (outSize == 0) return;
  if (transform == TextTransform::None) {
    snprintf(out, outSize, "%s", in);
    return;
  }
  size_t o = 0;
  bool atWordStart = true;
  const auto* p = reinterpret_cast<const unsigned char*>(in);
  uint32_t cp;
  while ((cp = utf8NextCodepoint(&p)) != 0) {
    uint32_t mapped = cp;
    switch (transform) {
      case TextTransform::Upper:
        mapped = toUpper(cp);
        break;
      case TextTransform::Lower:
        mapped = toLower(cp);
        break;
      case TextTransform::Capitalize:
        if (atWordStart) mapped = toUpper(cp);
        break;
      case TextTransform::None:
        break;
    }
    atWordStart = !isWordChar(cp);
    // Leave room for the terminator; stop at the first codepoint that won't fit
    // so a multi-byte sequence is never cut.
    const size_t n = encode(mapped, out + o, outSize - 1 - o);
    if (n == 0) break;
    o += n;
  }
  out[o] = '\0';
}

}  // namespace SleepThemeText
