#include "SpacedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstring>

namespace {

// Longest run handed to the renderer in one call. Words are far shorter; this only bounds
// the stack for unbroken input (a URL, CJK text without spaces).
constexpr size_t RUN_BUFFER_BYTES = 64;

uint32_t codepointAt(const char* p) {
  if (*p == '\0') return 0;
  const auto* cursor = reinterpret_cast<const unsigned char*>(p);
  return utf8NextCodepoint(&cursor);
}

// Last codepoint of the n-byte run at p (n > 0): back up over UTF-8 continuation bytes.
uint32_t lastCodepointOf(const char* p, const size_t n) {
  size_t i = n - 1;
  while (i > 0 && (static_cast<uint8_t>(p[i]) & 0xC0) == 0x80) --i;
  return codepointAt(p + i);
}

// Walks `text` once, optionally drawing it, and returns its total advance.
int walk(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
         const TextLayoutOptions& layout, const EpdFontFamily::Style style, const bool drawIt, const bool black) {
  if (!text) return 0;
  char run[RUN_BUFFER_BYTES];
  int pen = 0;
  uint32_t prevCp = 0;
  const char* p = text;
  while (*p != '\0') {
    if (*p == ' ') {
      pen += layout.scaleSpace(renderer.getSpaceAdvance(fontId, prevCp, codepointAt(p + 1), style));
      prevCp = ' ';
      ++p;
      continue;
    }
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ' ' && n < sizeof(run) - 1) ++n;
    if (p[n] != '\0' && p[n] != ' ') {
      // Buffer full mid-run: never cut inside a UTF-8 sequence.
      while (n > 1 && (static_cast<uint8_t>(p[n]) & 0xC0) == 0x80) --n;
    }
    memcpy(run, p, n);
    run[n] = '\0';
    if (drawIt) renderer.drawText(fontId, x + pen, y, run, black, style, layout.characterSpacing);
    pen += renderer.getTextAdvanceX(fontId, run, style, layout.characterSpacing);
    prevCp = lastCodepointOf(run, n);
    p += n;
  }
  return pen;
}

}  // namespace

int SpacedText::advance(const GfxRenderer& renderer, const int fontId, const char* text,
                        const TextLayoutOptions& layout, const EpdFontFamily::Style style) {
  return walk(renderer, fontId, 0, 0, text, layout, style, false, true);
}

void SpacedText::draw(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
                      const TextLayoutOptions& layout, const bool black, const EpdFontFamily::Style style) {
  walk(renderer, fontId, x, y, text, layout, style, true, black);
}
