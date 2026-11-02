#pragma once

#include <cstddef>
#include <string>

/**
 * Bounded plain-text excerpt of an OPDS book description (Atom summary/content,
 * OPDS 2 metadata.description), for the preview shown before a download.
 *
 * Text is fed in pieces as the parser delivers it. HTML tags are dropped (a
 * tag becomes a space), whitespace runs collapse to one space, and nothing is
 * kept past `maxBytes`, so the buffer never grows beyond the bound however
 * long the description is. finish() decodes the common HTML entities that an
 * escaped-HTML description still carries after XML/JSON decoding and trims a
 * cut UTF-8 sequence.
 */
class OpdsDescription {
 public:
  // Reserves the whole bound once: the only allocation, freed with the object.
  void begin(size_t maxBytes);
  void reset();  // empties the text for the next entry, keeps the reservation
  void append(const char* text, size_t len);
  // Separator between XHTML elements, so "<p>a</p><p>b</p>" reads "a b".
  void appendSpace() { append(" ", 1); }
  bool empty() const { return text.empty(); }
  bool full() const { return text.size() >= maxBytes; }
  // Cleans up and hands over the text; the object is reset.
  std::string finish();

 private:
  std::string text;
  size_t maxBytes = 0;
  bool inTag = false;
};
