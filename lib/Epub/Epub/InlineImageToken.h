#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// A small image that flows inside a paragraph travels through ParsedText as one "word": a
// 14-byte token, "\x01I" followed by three 4-digit hex fields (id, width, height). It takes
// part in line breaking like any word of that width, and is removed again when the line is
// extracted: the line's TextBlock never contains it, and the image itself is stored on the
// page as an ordinary PageImage.
//
// Encoding it as a word is what keeps the feature free for books without inline images:
// ParsedText's per-word arrays do not grow, and a paragraph with no token pays one byte
// comparison per word (isInlineImageToken's first test). 0x01 cannot occur in real text:
// XML 1.0 forbids it, so expat rejects a document that contains it.
constexpr size_t INLINE_IMAGE_TOKEN_BYTES = 14;

inline bool isInlineImageToken(const std::string& word) {
  return word.size() == INLINE_IMAGE_TOKEN_BYTES && word[0] == '\x01' && word[1] == 'I';
}

inline std::string makeInlineImageToken(const uint16_t id, const uint16_t width, const uint16_t height) {
  char token[INLINE_IMAGE_TOKEN_BYTES + 1];
  snprintf(token, sizeof(token), "\x01I%04X%04X%04X", id, width, height);
  return token;
}

// False unless `word` is a well-formed token.
inline bool parseInlineImageToken(const std::string& word, uint16_t& id, uint16_t& width, uint16_t& height) {
  if (!isInlineImageToken(word)) return false;
  uint16_t values[3] = {};
  for (int group = 0; group < 3; ++group) {
    uint16_t value = 0;
    for (int digit = 0; digit < 4; ++digit) {
      const char c = word[2 + group * 4 + digit];
      uint8_t nibble;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<uint8_t>(c - '0');
      } else if (c >= 'A' && c <= 'F') {
        nibble = static_cast<uint8_t>(c - 'A' + 10);
      } else {
        return false;
      }
      value = static_cast<uint16_t>((value << 4) | nibble);
    }
    values[group] = value;
  }
  id = values[0];
  width = values[1];
  height = values[2];
  return true;
}
