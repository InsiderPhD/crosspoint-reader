#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

// Sanity bound on one entry's body slice, used to reject a corrupt cache file and
// to cap the layout's split. A page's footnote area is capped at half the viewport
// (~2KB of text on the widest panel), so this is a guard rail, not a working limit.
constexpr size_t MAX_FOOTNOTE_TEXT_BYTES = 4096;

// One footnote reference on a page: its marker, its link target, and — in the
// on-page display mode — the slice of the body that this page shows.
//
// `text` is right-sized rather than a fixed `char[1024]`. With the fixed array a
// page carrying the maximum 16 notes reserved ~17KB of heap for text the page
// can never display more than ~2KB of, and the reader held that for as long as
// the page was on screen; on a tight heap that is the difference between the
// glyph cache getting its contiguous blocks and falling back to per-glyph decode.
//
// Move-only by consequence: entries are moved into the page and from the page
// into the reader, never copied. Anything that only needs the reference (the
// parser's pending and deferred lists) uses FootnoteRef instead.
struct FootnoteEntry {
  char number[24] = {};
  char href[64] = {};
  std::unique_ptr<char[]> text;

  bool hasText() const { return text != nullptr; }

  // Never null, so callers can hand it straight to a C string API.
  const char* textOrEmpty() const { return text ? text.get() : ""; }

  // Copies `len` bytes of `src`. On allocation failure the entry is simply left
  // without text: the note renders blank and its reference still works from the
  // footnotes menu, which beats aborting the device under -fno-exceptions.
  void setText(const char* src, const size_t len) {
    text.reset();
    if (!src || len == 0) return;
    text.reset(new (std::nothrow) char[len + 1]);
    if (!text) return;
    memcpy(text.get(), src, len);
    text[len] = '\0';
  }
};

// A footnote reference with no body of its own: what the anchor scan collects
// while it waits for the page its word lands on, and what a page-to-page
// deferral carries. `text`, when set, points into the parser's body pool at the
// byte the next page must resume from — the pool owns it, so neither list pays
// for a copy of a body it does not own.
struct FootnoteRef {
  char number[sizeof(FootnoteEntry::number)] = {};
  char href[sizeof(FootnoteEntry::href)] = {};
  const char* text = nullptr;
};
