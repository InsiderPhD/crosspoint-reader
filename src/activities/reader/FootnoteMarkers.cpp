#include "FootnoteMarkers.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint8_t MAX_NOTES = Page::MAX_FOOTNOTES_PER_PAGE;
constexpr uint8_t NONE = 0xFF;
// A label this long was cut by the parser's fixed buffer, so the link's text may
// run on past it.
constexpr size_t LABEL_CAPACITY = sizeof(FootnoteEntry::number) - 1;

static_assert(MAX_NOTES <= 16, "the marker mask is 16 bits wide");

// Next byte of `p` that takes part in a label comparison, or 0 at the end.
//
// The parser trims whitespace and square brackets off each chunk of link text
// before storing it as the label ("[ 12 ]" becomes "12"), while the layout splits
// the same text into words at whitespace and keeps the brackets. Dropping both on
// each side makes the two comparable. U+2003 is the paragraph-indent prefix the
// layout glues onto a paragraph's first word.
char nextKept(const char*& p) {
  for (;;) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == 0) return 0;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '[' || c == ']') {
      ++p;
      continue;
    }
    if (c == 0xE2 && static_cast<unsigned char>(p[1]) == 0x80 && static_cast<unsigned char>(p[2]) == 0x83) {
      p += 3;
      continue;
    }
    ++p;
    return static_cast<char>(c);
  }
}

class Scanner {
 public:
  Scanner(const Page& page, const FootnoteMarkers::WordFn fn, void* ctx)
      : page(page), fn(fn), ctx(ctx), count(static_cast<uint8_t>(std::min<size_t>(page.footnotes.size(), MAX_NOTES))) {}

  uint16_t run() {
    if (count == 0) return 0;

    // Footnotes sharing a label ("*" on every note is common) form a class, named
    // by its first member.
    for (uint8_t i = 0; i < count; ++i) {
      classOf[i] = i;
      for (uint8_t j = 0; j < i; ++j) {
        if (strcmp(page.footnotes[j].number, page.footnotes[i].number) == 0) {
          classOf[i] = j;
          break;
        }
      }
    }

    assigning = false;
    walk();

    // A class with more entries than markers has entries whose reference is not
    // on this page. Those are the carried-over body tails, and the layout puts
    // them first, so the markers belong to the LAST entries of the class. Getting
    // this wrong would send a "*" to the previous page's note.
    uint16_t mask = 0;
    for (uint8_t cls = 0; cls < count; ++cls) {
      if (classOf[cls] != cls) continue;
      uint8_t entries = 0;
      for (uint8_t i = cls; i < count; ++i) entries += classOf[i] == cls;
      const uint8_t found = markers[cls];
      uint8_t skip = entries > found ? static_cast<uint8_t>(entries - found) : 0;
      uint8_t left = found;
      next[cls] = NONE;
      for (uint8_t i = cls; i < count && left > 0; ++i) {
        if (classOf[i] != cls) continue;
        if (skip > 0) {
          --skip;
          continue;
        }
        if (next[cls] == NONE) next[cls] = i;
        mask |= static_cast<uint16_t>(1U << i);
        --left;
      }
    }

    if (fn && mask != 0) {
      assigning = true;
      walk();
    }
    return mask;
  }

 private:
  enum class Cmp : uint8_t { Mismatch, Prefix, Equal };

  // How the run's text so far relates to `label`. `cut` reports a label that
  // filled the parser's buffer and matched all the way: the link may be longer.
  Cmp compare(const char* label, bool& cut) const {
    const char* p = label;
    const bool full = strlen(label) >= LABEL_CAPACITY;
    for (uint8_t i = 0; i < accLen; ++i) {
      const char c = nextKept(p);
      if (c == 0) {
        cut = full;
        return full ? Cmp::Equal : Cmp::Mismatch;
      }
      if (c != acc[i]) return Cmp::Mismatch;
    }
    if (nextKept(p) != 0) return Cmp::Prefix;
    cut = full;
    return Cmp::Equal;
  }

  void endRun() {
    accLen = 0;
    dead = false;
    absorbing = false;
    segBlock = nullptr;
  }

  void emit(const uint8_t entry, const TextBlock& block, const uint16_t first, const uint16_t last, const int x,
            const int y) const {
    if (!assigning || !fn || entry == NONE) return;
    for (uint16_t i = first; i <= last; ++i) {
      const FootnoteMarkers::Word word{entry, &block, i, static_cast<int16_t>(x + block.wordXpos(i)),
                                       static_cast<int16_t>(y)};
      fn(ctx, word);
    }
  }

  void onMatch(const uint8_t cls, const bool cut, const TextBlock& block, const uint16_t last, const int x,
               const int y) {
    uint8_t entry = NONE;
    if (assigning) {
      entry = next[cls];
      if (entry != NONE) {
        next[cls] = NONE;
        for (uint8_t i = entry + 1; i < count; ++i) {
          if (classOf[i] == cls) {
            next[cls] = i;
            break;
          }
        }
      }
      if (segBlock == &block) {
        emit(entry, block, segFirst, last, x, y);
      } else {
        // A link wrapped onto this line: the head of it is the tail of the line
        // it started on. (Whole lines in between are not highlighted; a three-line
        // footnote link is not worth carrying more state for.)
        if (segBlock && segBlock->wordCount() > 0) {
          emit(entry, *segBlock, segFirst, static_cast<uint16_t>(segBlock->wordCount() - 1), segX, segY);
        }
        emit(entry, block, 0, last, x, y);
      }
    } else if (markers[cls] < UINT8_MAX) {
      ++markers[cls];
    }
    accLen = 0;
    segBlock = nullptr;
    absorbing = cut;
    absorbEntry = entry;
  }

  void feedBlock(const TextBlock& block, const int x, const int y) {
    const uint16_t words = block.wordCount();
    for (uint16_t i = 0; i < words; ++i) {
      if ((block.wordStyle(i) & EpdFontFamily::UNDERLINE) == 0) {
        endRun();
        continue;
      }
      if (absorbing) {
        emit(absorbEntry, block, i, i, x, y);
        continue;
      }
      if (dead) continue;

      const uint8_t before = accLen;
      const char* p = block.wordText(i);
      for (char c = nextKept(p); c != 0; c = nextKept(p)) {
        if (accLen == sizeof(acc)) {
          dead = true;  // longer than any label can be
          break;
        }
        acc[accLen++] = c;
      }
      if (dead || accLen == before) continue;  // an all-blank word neither starts nor breaks a run
      if (!segBlock) {
        segBlock = &block;
        segFirst = i;
        segX = static_cast<int16_t>(x);
        segY = static_cast<int16_t>(y);
      }

      bool couldGrow = false;
      uint8_t matched = NONE;
      bool cut = false;
      for (uint8_t cls = 0; cls < count && matched == NONE; ++cls) {
        if (classOf[cls] != cls) continue;
        switch (compare(page.footnotes[cls].number, cut)) {
          case Cmp::Equal:
            matched = cls;
            break;
          case Cmp::Prefix:
            couldGrow = true;
            break;
          case Cmp::Mismatch:
            break;
        }
      }
      if (matched != NONE) {
        onMatch(matched, cut, block, i, x, y);
      } else if (!couldGrow) {
        dead = true;  // underlined text that is no footnote's label (a <u>, a chapter link)
      }
    }
  }

  // Mirrors PageTableFragment::render's cell geometry.
  void walkTable(const PageTableFragment& table) {
    const uint8_t columns = table.getColumnCount();
    if (columns == 0) return;
    int rowY = table.yPos;
    for (const TableRow& row : table.getRows()) {
      uint8_t col = 0;
      for (const TableCell& cell : row.cells) {
        if (col >= columns) break;
        const uint8_t span = std::min<uint8_t>(cell.colSpan ? cell.colSpan : 1, columns - col);
        const int cellX = table.xPos +
                          static_cast<int>((static_cast<uint32_t>(table.getTotalWidth()) * col) / columns) +
                          TABLE_CELL_PADDING;
        int lineY = rowY + TABLE_CELL_PADDING;
        endRun();  // a link never continues from one cell into the next
        for (const auto& line : cell.lines) {
          if (line && line->valid()) feedBlock(*line, cellX, lineY);
          lineY += table.getLineStep();
        }
        col = static_cast<uint8_t>(col + span);
      }
      rowY += row.height;
    }
    endRun();
  }

  void walk() {
    endRun();
    for (const auto& element : page.elements) {
      if (!element) continue;
      switch (element->getTag()) {
        case TAG_PageLine: {
          const auto& line = static_cast<const PageLine&>(*element);
          // Consecutive lines are NOT separated by endRun(): a link that wraps
          // carries its text across the break.
          if (line.getBlock() && line.getBlock()->valid()) feedBlock(*line.getBlock(), line.xPos, line.yPos);
          break;
        }
        case TAG_PageTable:
          walkTable(static_cast<const PageTableFragment&>(*element));
          break;
        case TAG_PageImage:
        case TAG_PageHorizontalRule:
          endRun();
          break;
        case TAG_PageBorderBox:
          // A frame around text, not something between two lines of it. (Boxes and inline
          // images sit at the front of the element list, ahead of every line.)
          break;
      }
    }
  }

  const Page& page;
  const FootnoteMarkers::WordFn fn;
  void* const ctx;
  const uint8_t count;

  bool assigning = false;
  uint8_t classOf[MAX_NOTES] = {};
  uint8_t markers[MAX_NOTES] = {};  // per class: markers found by the counting pass
  uint8_t next[MAX_NOTES] = {};     // per class: the entry the next marker belongs to

  // The underlined run being read: its text so far, and where it started.
  char acc[32] = {};
  uint8_t accLen = 0;
  bool dead = false;       // the run matches no label; skip to its end
  bool absorbing = false;  // the run matched a cut label; the rest of it is the same marker
  uint8_t absorbEntry = NONE;
  const TextBlock* segBlock = nullptr;
  uint16_t segFirst = 0;
  int16_t segX = 0;
  int16_t segY = 0;
};

}  // namespace

uint16_t FootnoteMarkers::scan(const Page& page, const WordFn fn, void* ctx) { return Scanner(page, fn, ctx).run(); }
