#include "OpdsDescription.h"

#include <cstdlib>
#include <cstring>

namespace {
bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// UTF-8 for a code point; returns the byte count written to out (<= 4).
size_t encodeUtf8(unsigned long cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp > 0x10FFFF) return 0;
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

struct NamedEntity {
  const char* name;
  const char* text;
};
constexpr NamedEntity NAMED_ENTITIES[] = {
    {"amp", "&"},
    {"lt", "<"},
    {"gt", ">"},
    {"quot", "\""},
    {"apos", "'"},
    {"nbsp", " "},
    {"rsquo", "\xE2\x80\x99"},
    {"lsquo", "\xE2\x80\x98"},
    {"rdquo", "\xE2\x80\x9D"},
    {"ldquo", "\xE2\x80\x9C"},
    {"mdash", "\xE2\x80\x94"},
    {"ndash", "\xE2\x80\x93"},
    {"hellip", "\xE2\x80\xA6"},
};

// In place: an entity never decodes to more bytes than its own spelling.
void decodeEntities(std::string& s) {
  size_t out = 0;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      const size_t semi = s.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        const std::string name = s.substr(i + 1, semi - i - 1);
        char buf[4];
        size_t n = 0;
        if (name.size() > 1 && name[0] == '#') {
          const bool hex = name[1] == 'x' || name[1] == 'X';
          const unsigned long cp = strtoul(name.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
          if (cp) n = encodeUtf8(cp, buf);
        } else {
          for (const auto& e : NAMED_ENTITIES) {
            if (name == e.name) {
              n = strlen(e.text);
              memcpy(buf, e.text, n);
              break;
            }
          }
        }
        if (n > 0 && n <= semi - i + 1) {
          memcpy(&s[out], buf, n);
          out += n;
          i = semi + 1;
          continue;
        }
      }
    }
    s[out++] = s[i++];
  }
  s.resize(out);
}
}  // namespace

void OpdsDescription::begin(const size_t bytes) {
  maxBytes = bytes;
  text.reserve(bytes);
  reset();
}

void OpdsDescription::reset() {
  text.clear();
  inTag = false;
}

void OpdsDescription::append(const char* s, const size_t len) {
  for (size_t i = 0; i < len && text.size() < maxBytes; ++i) {
    const char c = s[i];
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (inTag) {
      if (c == '>') {
        inTag = false;
        if (!text.empty() && text.back() != ' ') text.push_back(' ');
      }
      continue;
    }
    if (isSpace(c)) {
      if (!text.empty() && text.back() != ' ') text.push_back(' ');
      continue;
    }
    text.push_back(c);
  }
}

std::string OpdsDescription::finish() {
  decodeEntities(text);
  // Whitespace again: &nbsp; and friends may have made new runs.
  while (!text.empty() && text.back() == ' ') text.pop_back();
  // A cut multi-byte sequence at the end.
  if (!text.empty() && (static_cast<unsigned char>(text.back()) & 0x80)) {
    size_t start = text.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
    const auto lead = static_cast<unsigned char>(text[start]);
    const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (text.size() - start < need) text.resize(start);
  }
  std::string out = std::move(text);
  text = std::string();
  inTag = false;
  return out;
}
