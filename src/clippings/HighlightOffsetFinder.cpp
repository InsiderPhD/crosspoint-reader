#include "HighlightOffsetFinder.h"

#include <Logging.h>
#include <Memory.h>
#include <Print.h>

#include <cstdlib>
#include <cstring>

namespace HighlightOffsetFinder {
namespace {

// Characters the reader's clip text can't be trusted to preserve verbatim
// (collapsed/inserted whitespace, soft and inserted hyphens). Skipped on both
// sides of the match; they still count toward the UTF-16 offsets.
bool isIgnored(const uint32_t cp) {
  switch (cp) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '-':
    case 0x00A0:  // nbsp
    case 0x00AD:  // soft hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x202F:  // narrow nbsp
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200B;  // en/em/thin/hair spaces, ZWSP
  }
}

uint32_t foldCase(const uint32_t cp) { return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp; }

size_t encodeUtf8(const uint32_t cp, uint8_t* out) {
  if (cp < 0x80) {
    out[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
    out[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
    out[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
  out[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  return 4;
}

// Incremental UTF-8 decoder. A malformed byte decodes as U+FFFD so offsets
// keep advancing one unit per bad byte.
struct Utf8Decoder {
  uint32_t cp = 0;
  uint8_t remaining = 0;

  // Returns true when `out` holds a complete code point.
  bool feed(const uint8_t b, uint32_t& out) {
    if (remaining > 0) {
      if ((b & 0xC0) == 0x80) {
        cp = (cp << 6) | (b & 0x3F);
        if (--remaining == 0) {
          out = cp;
          return true;
        }
        return false;
      }
      remaining = 0;  // truncated sequence; fall through and treat b as a lead byte
    }
    if (b < 0x80) {
      out = b;
      return true;
    }
    if ((b & 0xE0) == 0xC0) {
      cp = b & 0x1F;
      remaining = 1;
    } else if ((b & 0xF0) == 0xE0) {
      cp = b & 0x0F;
      remaining = 2;
    } else if ((b & 0xF8) == 0xF0) {
      cp = b & 0x07;
      remaining = 3;
    } else {
      out = 0xFFFD;
      return true;
    }
    return false;
  }
};

struct NamedEntity {
  const char* name;
  uint32_t cp;
};
constexpr NamedEntity NAMED_ENTITIES[] = {
    {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},      {"apos", '\''},
    {"nbsp", 0x00A0},  {"shy", 0x00AD},   {"ndash", 0x2013}, {"mdash", 0x2014},  {"lsquo", 0x2018},
    {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"hellip", 0x2026}, {"copy", 0x00A9},
};

// Print sink fed the raw chapter XHTML by Epub::readItemContentsToStream.
// Tracks the UTF-16 offset of <body> text content and runs KMP over the
// normalised (ignored chars dropped, ASCII folded) UTF-8 stream.
class OffsetMatcher final : public Print {
 public:
  OffsetMatcher(const uint8_t* pattern, const uint16_t* failure, uint32_t* startRing, const size_t patternLen,
                const size_t expectedRawByte)
      : pattern(pattern), failure(failure), startRing(startRing), m(patternLen), expectedRawByte(expectedRawByte) {}

  size_t write(const uint8_t b) override {
    consume(b);
    ++rawByte;
    return 1;
  }

  size_t write(const uint8_t* buffer, const size_t size) override {
    for (size_t i = 0; i < size; ++i) write(buffer[i]);
    return size;
  }

  bool found() const { return hasMatch; }
  uint32_t start() const { return bestStart; }
  uint32_t end() const { return bestEnd; }

 private:
  enum class Mode : uint8_t { Text, Tag, Comment, Entity, Done };

  void consume(const uint8_t c) {
    switch (mode) {
      case Mode::Done:
        return;
      case Mode::Tag:
        consumeTag(c);
        return;
      case Mode::Comment:
        // "-->" closes; count consecutive dashes before '>'.
        if (c == '>' && dashes >= 2) {
          mode = Mode::Text;
        }
        dashes = (c == '-') ? static_cast<uint8_t>(dashes + 1) : 0;
        return;
      case Mode::Entity:
        consumeEntity(c);
        return;
      case Mode::Text:
        if (c == '<') {
          beginTag();
        } else if (!inBody) {
          // <head> content isn't part of body text.
        } else if (c == '&') {
          mode = Mode::Entity;
          entityLen = 0;
        } else {
          uint32_t cp;
          if (decoder.feed(c, cp)) emit(cp);
        }
        return;
    }
  }

  void beginTag() {
    mode = Mode::Tag;
    tagLen = 0;
    tagNameDone = false;
    tagQuote = 0;
  }

  void consumeTag(const uint8_t c) {
    if (tagQuote != 0) {
      if (c == tagQuote) tagQuote = 0;
      return;
    }
    if (!tagNameDone) {
      const bool nameChar = c != '>' && c != ' ' && c != '\t' && c != '\n' && c != '\r' && !(c == '/' && tagLen > 0);
      if (nameChar) {
        if (tagLen < sizeof(tagName)) tagName[tagLen] = static_cast<char>(c | 0x20);  // ASCII lower-case
        ++tagLen;
        if (tagLen == 3 && memcmp(tagName, "!--", 3) == 0) {
          mode = Mode::Comment;
          dashes = 0;
        }
        return;
      }
      tagNameDone = true;
    }
    if (c == '"' || c == '\'') {
      tagQuote = c;
    } else if (c == '>') {
      mode = Mode::Text;
      if (tagLen == 4 && memcmp(tagName, "body", 4) == 0) {
        inBody = true;
      } else if (tagLen == 5 && memcmp(tagName, "/body", 5) == 0) {
        mode = Mode::Done;
      }
    }
  }

  void consumeEntity(const uint8_t c) {
    if (c == ';') {
      mode = Mode::Text;
      emit(decodeEntity());
      return;
    }
    if (entityLen < sizeof(entity) && c != '<' && c != '&' && c != ' ' && c != '\n') {
      entity[entityLen++] = static_cast<char>(c);
      return;
    }
    // Not an entity after all: the '&' and what followed are literal text.
    mode = Mode::Text;
    emit('&');
    for (uint8_t i = 0; i < entityLen; ++i) emit(static_cast<uint8_t>(entity[i]));
    consume(c);
  }

  uint32_t decodeEntity() const {
    char name[sizeof(entity) + 1];
    memcpy(name, entity, entityLen);
    name[entityLen] = '\0';
    if (name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const unsigned long v = strtoul(name + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
      return (v == 0 || v > 0x10FFFF) ? 0xFFFD : static_cast<uint32_t>(v);
    }
    for (const auto& e : NAMED_ENTITIES) {
      if (strcmp(e.name, name) == 0) return e.cp;
    }
    return 0xFFFD;
  }

  void emit(uint32_t cp) {
    // XML normalises CRLF to LF, so a CR adds nothing to the text content.
    if (cp == '\r') return;
    const uint32_t cpStart = utf16Pos;
    utf16Pos += cp >= 0x10000 ? 2 : 1;
    if (isIgnored(cp)) return;
    cp = foldCase(cp);
    uint8_t bytes[4];
    const size_t n = encodeUtf8(cp, bytes);
    for (size_t i = 0; i < n; ++i) feedKmp(bytes[i], cpStart);
  }

  void feedKmp(const uint8_t b, const uint32_t cpStart) {
    startRing[normPos % m] = cpStart;
    ++normPos;
    while (q > 0 && pattern[q] != b) q = failure[q - 1];
    if (pattern[q] == b) ++q;
    if (q < m) return;

    // Full match ending on this byte. utf16Pos is already past its code point.
    const uint32_t matchStart = startRing[(normPos - m) % m];
    const size_t distance = rawByte > expectedRawByte ? rawByte - expectedRawByte : expectedRawByte - rawByte;
    if (!hasMatch || distance < bestDistance) {
      hasMatch = true;
      bestDistance = distance;
      bestStart = matchStart;
      bestEnd = utf16Pos;
    }
    q = failure[q - 1];
  }

  const uint8_t* pattern;
  const uint16_t* failure;
  uint32_t* startRing;
  const size_t m;
  const size_t expectedRawByte;

  Mode mode = Mode::Text;
  bool inBody = false;
  Utf8Decoder decoder;
  char tagName[8] = {};
  size_t tagLen = 0;
  bool tagNameDone = false;
  uint8_t tagQuote = 0;
  uint8_t dashes = 0;
  char entity[10] = {};
  uint8_t entityLen = 0;

  size_t rawByte = 0;
  uint32_t utf16Pos = 0;
  size_t normPos = 0;
  size_t q = 0;

  bool hasMatch = false;
  size_t bestDistance = 0;
  uint32_t bestStart = 0;
  uint32_t bestEnd = 0;
};

}  // namespace

Result find(const Epub& epub, const int spineIndex, const std::string& clipText, const float expectedFraction,
            uint32_t& outStart, uint32_t& outEnd) {
  const std::string href = epub.getSpineItem(spineIndex).href;
  if (href.empty()) {
    LOG_ERR("HLO", "No spine item %d", spineIndex);
    return Result::Error;
  }

  // Normalised pattern is never longer than the clip text (ignored chars only
  // shrink it; case folding keeps byte length). Clip text is capped at
  // CLIPPING_TEXT_MAX (512), so these three buffers total <= ~3.5KB. Heap, not
  // stack: well past the 256-byte local budget. Freed on return.
  const size_t cap = clipText.size();
  if (cap == 0) return Result::NotFound;
  auto pattern = makeUniqueNoThrow<uint8_t[]>(cap);
  auto failure = makeUniqueNoThrow<uint16_t[]>(cap);
  auto startRing = makeUniqueNoThrow<uint32_t[]>(cap);
  if (!pattern || !failure || !startRing) {
    LOG_ERR("HLO", "Pattern alloc failed (%u bytes)", static_cast<unsigned>(cap * 7));
    return Result::Error;
  }

  size_t m = 0;
  Utf8Decoder decoder;
  for (const char ch : clipText) {
    uint32_t cp;
    if (!decoder.feed(static_cast<uint8_t>(ch), cp) || isIgnored(cp)) continue;
    uint8_t bytes[4];
    const size_t n = encodeUtf8(foldCase(cp), bytes);
    if (m + n > cap) break;  // U+FFFD re-encoding of a bad byte can grow it
    memcpy(pattern.get() + m, bytes, n);
    m += n;
  }
  if (m == 0) return Result::NotFound;

  failure[0] = 0;
  for (size_t i = 1, k = 0; i < m; ++i) {
    while (k > 0 && pattern[i] != pattern[k]) k = failure[k - 1];
    if (pattern[i] == pattern[k]) ++k;
    failure[i] = static_cast<uint16_t>(k);
  }

  size_t itemSize = 0;
  epub.getItemSize(href, &itemSize);
  float fraction = expectedFraction < 0.0f ? 0.0f : (expectedFraction > 1.0f ? 1.0f : expectedFraction);
  const auto expectedRawByte = static_cast<size_t>(fraction * static_cast<float>(itemSize));

  OffsetMatcher matcher(pattern.get(), failure.get(), startRing.get(), m, expectedRawByte);
  if (!epub.readItemContentsToStream(href, matcher, 1024)) {
    LOG_ERR("HLO", "Failed to read %s", href.c_str());
    return Result::Error;
  }
  if (!matcher.found()) {
    LOG_INF("HLO", "Clip text not found in spine %d (%u pattern bytes)", spineIndex, static_cast<unsigned>(m));
    return Result::NotFound;
  }
  outStart = matcher.start();
  outEnd = matcher.end();
  LOG_DBG("HLO", "Spine %d: offsets %lu-%lu", spineIndex, static_cast<unsigned long>(outStart),
          static_cast<unsigned long>(outEnd));
  return Result::Found;
}

}  // namespace HighlightOffsetFinder
