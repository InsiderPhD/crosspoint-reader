#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>

// Streaming Print filter that makes HTML void elements well-formed XML on
// their way to the chapter's temp file: "<img ...>" becomes "<img .../>" and a
// stray closer such as "</br>" or "</img>" is dropped. Expat is a strict XML
// parser, and one unclosed <img> (BookFusion web-article exports emit them)
// otherwise fails the whole chapter with "mismatched tag" at the parent's
// closing tag.
//
// Every other byte passes through untouched, in runs straight from the
// caller's buffer: no heap, and only a few bytes of lookahead held across
// chunk boundaries (the "</name" of a closing tag, until it is known whether
// to drop it). Comments, CDATA, declarations and processing instructions are
// passed through without being scanned for tags.
class VoidTagCloser final : public Print {
 public:
  explicit VoidTagCloser(Print& out) : out(out) {}

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t size) override;

  // Emits any held lookahead (input that ended mid-tag). Returns false if any
  // write to the underlying stream came up short.
  bool finish();

 private:
  enum class State : uint8_t {
    Text,
    TagOpen,     // after '<'
    TagName,     // inside the element name
    TagBody,     // attributes, up to the closing '>'
    TagQuoted,   // inside a quoted attribute value
    DropCloser,  // discarding "</void ...>"
    Bang,        // after "<!"
    BangDash,    // after "<!-"
    Comment,     // inside "<!-- ... -->"
    CData,       // inside "<![ ... ]]>"
    Decl,        // inside "<!DOCTYPE ...>" and friends
    Pi,          // inside "<? ... ?>"
  };

  // "</" + the longest void element name ("source"/"param"/"track" are 6).
  static constexpr size_t MAX_HELD = 8;
  static constexpr size_t MAX_NAME = 6;

  bool emit(const uint8_t* data, size_t len);
  bool isVoidName() const;

  Print& out;
  State state = State::Text;
  bool closing = false;
  bool ok = true;
  uint8_t quote = 0;
  uint8_t prevSignificant = 0;  // last non-space byte of the tag body
  uint8_t markupRun = 0;        // consecutive '-' (Comment) or ']' (CData)
  uint8_t nameLen = 0;          // MAX_NAME + 1 means "too long to be void"
  char name[MAX_NAME] = {};
  uint8_t held[MAX_HELD] = {};
  uint8_t heldLen = 0;
};
