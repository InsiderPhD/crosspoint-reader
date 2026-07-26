#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// A single laid-out word on a reader page, with the on-screen geometry needed to draw a
// selection cursor/highlight over it. Built transiently in EpubReaderActivity::startClipSelection
// from the live page layout and consumed by ClipSelectionActivity / ClipTextBuilder.
//
// Deliberately 14 bytes with the text held out-of-line in WordList::textPool: a dense 3-page
// selection span is ~900 words, and an earlier per-word std::string layout (~48 bytes each)
// needed a ~46KB contiguous vector block that abort()ed on the fragmented reader heap.
struct WordRef {
  uint16_t textOffset = 0;  // offset of this word's null-terminated text in WordList::textPool
  int16_t x = 0;            // screen coords fit int16_t (display is 800x480 plus small margins)
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  uint8_t pageIdx = 0;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;
  bool endsWithInsertedHyphen = false;
};

// The selectable words of up to 3 reader pages plus their pooled text. Each word's text is
// appended to textPool followed by '\0', so textOf() returns a valid C string usable directly
// at C API boundaries (drawText, getTextAdvanceX). textOffset is uint16_t, so the pool is
// capped at 64KB by the builder (3 pages of text is ~12KB worst case).
struct WordList {
  std::vector<WordRef> words;
  std::string textPool;

  const char* textOf(const WordRef& word) const { return textPool.c_str() + word.textOffset; }
};

namespace clipword {

// True when the word text begins with U+2003 EM SPACE (the layout engine's paragraph-indent
// marker). Safe on any null-terminated string: the comparison chain short-circuits at '\0'.
inline bool hasEmSpacePrefix(const char* text) {
  return static_cast<unsigned char>(text[0]) == 0xE2 && static_cast<unsigned char>(text[1]) == 0x80 &&
         static_cast<unsigned char>(text[2]) == 0x83;
}

// True when the text contains only ASCII whitespace (or is empty).
inline bool isBlank(const char* text) { return text[strspn(text, " \t\r\n")] == '\0'; }

}  // namespace clipword
