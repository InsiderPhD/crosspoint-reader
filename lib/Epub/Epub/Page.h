#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageHorizontalRule = 3,
  TAG_PageTable = 4,  // persisted in section.bin: never renumber
};

// Table grid limits (ported from witchhunt-reader). Every fragment is also bounded by the
// viewport height, so a fragment never holds more than one page of cell lines.
static constexpr uint8_t MAX_TABLE_COLS = 8;
static constexpr uint16_t MAX_TABLE_ROWS = 48;  // per FRAGMENT; the packer enforces it
static constexpr uint8_t TABLE_CELL_PADDING = 5;
static constexpr uint16_t MIN_COL_INNER_WIDTH = 24;
static constexpr uint8_t MAX_CELL_LINES = 64;

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(FsFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  // unique_ptr, not shared_ptr: a TextBlock is produced by the layout and handed to
  // exactly one PageLine, which the Page owns until it is dropped. shared_ptr bought
  // nothing but an atomic refcount and a per-object control block on a single-core
  // RISC-V part (see CLAUDE.md's memory rules).
  std::unique_ptr<TextBlock> block;

 public:
  PageLine(std::unique_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::unique_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  // Single-owner, same reasoning as PageLine::block above.
  std::unique_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::unique_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  // Outline box in the image's reserved layout space, for the pre-decode pass.
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset) const;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(FsFile& file);
};

struct TableCell {
  std::vector<std::unique_ptr<TextBlock>> lines;
  bool isHeader = false;
  // Grid columns this cell covers. A row's spans always sum to the fragment's columnCount
  // (layout pads short rows), so the renderer walks cells and accumulates.
  uint8_t colSpan = 1;
};

struct TableRow {
  std::vector<TableCell> cells;
  uint16_t height = 0;       // content + 2*TABLE_CELL_PADDING
  bool isHeaderRow = false;  // 2px separator below
};

// One page's slice of a table: a bordered grid of equal-width columns. A table taller than
// the viewport is split into several fragments at row boundaries (see ChapterHtmlSlimParser).
class PageTableFragment final : public PageElement {
  uint8_t columnCount = 0;
  // Vertical step between cell lines, fixed at layout time so it includes lineCompression
  // (witchhunt re-derives plain getLineHeight() at render, which overflows cells on Tight spacing).
  uint16_t lineStep = 0;
  uint16_t totalWidth = 0;
  uint16_t totalHeight = 0;
  bool hasBorder = true;
  std::vector<TableRow> rows;

 public:
  PageTableFragment(uint8_t colCount, uint16_t lineStep, uint16_t totalWidth, uint16_t totalHeight,
                    std::vector<TableRow> rows, int16_t xPos, int16_t yPos, bool hasBorder)
      : PageElement(xPos, yPos),
        columnCount(colCount),
        lineStep(lineStep),
        totalWidth(totalWidth),
        totalHeight(totalHeight),
        hasBorder(hasBorder),
        rows(std::move(rows)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageTable; }
  static std::unique_ptr<PageTableFragment> deserialize(FsFile& file);
  uint16_t getTotalHeight() const { return totalHeight; }
  const std::vector<TableRow>& getRows() const { return rows; }
};

class Page {
 public:
  // the list of block index and line numbers on this page.
  // unique_ptr: the Page is the sole owner of every element it holds; nothing
  // copies an element out (all consumers iterate by const reference).
  std::vector<std::unique_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;

  void addFootnote(const char* number, const char* href, const char* text = nullptr) {
    addFootnote(number, href, text, text ? strlen(text) : 0);
  }

  // `textLen` bytes of `text` only — the layout hands over one page's worth of a
  // footnote body at a time and keeps the rest for the next page, so the copy must
  // stop where it was told to, not at the end of the string.
  void addFootnote(const char* number, const char* href, const char* text, size_t textLen) {
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
    FootnoteEntry entry;
    strncpy(entry.number, number, sizeof(entry.number) - 1);
    strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.setText(text, textLen);
    footnotes.push_back(std::move(entry));
  }

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  // Images only. The grayscale planes use this when text anti-aliasing is off:
  // the page still needs its images composed into both planes, but re-rendering
  // every glyph per band would cost the full AA price for no AA.
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  // Text as usual, outline boxes where the images will land. Displayed once
  // before a decode so the reader sees the page instead of the previous one
  // while a multi-second decode runs.
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  // Renders footnote rule + text at the bottom of the viewport (no-op if no footnotes have text)
  void renderFootnotes(GfxRenderer& renderer, int fontId, int xOffset, int viewportBottom, int viewportWidth) const;
  // Returns the number of display lines needed to word-wrap `text` into `maxWidth` pixels
  static int countWrappedLines(GfxRenderer& renderer, int fontId, const char* text, int maxWidth);
  // Returns the byte offset in `text` where the (maxLines+1)-th wrapped line would begin.
  // Returns strlen(text) if the text fits in maxLines.
  static size_t splitWrappedAtLine(GfxRenderer& renderer, int fontId, const char* text, int maxLines, int maxWidth);
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::unique_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }

  // True when at least one image still has to be decoded (no usable .pxc yet),
  // i.e. this page view will pay a multi-second decode and is worth showing
  // placeholders for first. Touches the SD once per image, so call it once per
  // page render, not per pass.
  bool hasImagesNeedingDecode() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& el) {
      return el->getTag() == TAG_PageImage && static_cast<const PageImage&>(*el).getImageBlock().needsDecode();
    });
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
