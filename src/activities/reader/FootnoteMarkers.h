#pragma once

#include <cstdint>

class Page;
class TextBlock;

// Finds where each of a page's footnote references is drawn.
//
// The section cache stores a page's footnotes (label + href) and its words
// separately, with no link between the two: upstream added per-page link
// rectangles for this and CrossInk a link id on every word, and both grow the
// serialized page. This works from what the page already holds instead. The
// parser underlines every word of an internal link and records the link's text
// as the footnote's label, so an underlined run whose text is a footnote's label
// IS that footnote's marker. Nothing is stored: the answer is recomputed from the
// page whenever it is needed, on the stack.
namespace FootnoteMarkers {

// One word of a located marker. x/y are the word's top-left corner in page
// coordinates, i.e. before the reader's margins are added.
struct Word {
  uint8_t footnote;  // index into Page::footnotes
  const TextBlock* block;
  uint16_t index;  // word within `block`
  int16_t x;
  int16_t y;
};

// Function pointer + context rather than std::function (see CLAUDE.md).
using WordFn = void (*)(void* ctx, const Word& word);

// Returns a mask with bit i set when Page::footnotes[i] has a marker on the page.
// With `fn`, also reports every word of every located marker, in page order.
//
// A footnote without a marker is normal, not an error: the tail of a body carried
// over from the previous page has its reference there, and a reference inside a
// table cell that could not be told apart from its neighbours is left out rather
// than guessed at.
uint16_t scan(const Page& page, WordFn fn = nullptr, void* ctx = nullptr);

}  // namespace FootnoteMarkers
