#pragma once

#include <expat.h>

#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../FootnoteEntry.h"

// Offset sentinel for a FootnoteBodyEntry with no body text in the pool.
constexpr uint16_t FOOTNOTE_POOL_NO_TEXT = 0xFFFF;

// Packed backing store for footnote body text, shared by every FootnoteBodyEntry.
//
// This used to be a `char text[1024]` inline in each entry: 32 x ~1090 bytes was
// ~35KB in ONE contiguous block, the largest single allocation a section build
// made, and a hard build abort whenever the heap could not produce that block.
// Footnote bodies are typically 50-200 bytes, so reserving the 1024-byte worst
// case 32 times over wasted the great majority of it. Packing the bodies
// back-to-back in one modest buffer and storing a 2-byte offset per entry cuts
// the largest block to POOL_BYTES and the total to ~14KB.
//
// The pool is a FIXED size deliberately. A pool that shrank under heap pressure
// would make "which footnotes fit" depend on the heap state at build time, and
// Section.cpp stamps the *requested* footnoteDisplay into the cache header — so
// a memory-dependent result would be cached and never invalidated. Fixed size
// means exhaustion depends only on the chapter's own content.
class FootnoteBodyPool {
 public:
  // 32 entries averaging ~380 bytes. Bodies past this fall back to off-page
  // display for the rest of the chapter (logged by the caller).
  static constexpr uint16_t POOL_BYTES = 12 * 1024;

  // nothrow: a throwing new under -fno-exceptions goes straight to abort().
  bool allocate() {
    buffer.reset(new (std::nothrow) char[POOL_BYTES]);
    used = 0;
    return buffer != nullptr;
  }
  bool valid() const { return buffer != nullptr; }
  void reset() {
    buffer.reset();
    used = 0;
  }
  uint16_t bytesUsed() const { return used; }

  // Per-body ceiling. Bodies are packed into one shared buffer, so without a ceiling
  // a single footnote link pointing at a large id'd container (a whole <div> of notes,
  // say) would swallow the pool and starve every later footnote in the chapter.
  // 4KB is roughly three screenfuls of footnote area — far beyond any real note.
  static constexpr uint16_t MAX_BODY_BYTES = 4 * 1024;

  // The pre-scan accumulates body text directly into the unused tail of the pool
  // instead of a fixed scratch buffer, then commits it with commitCapture(). An
  // abandoned capture costs nothing: `used` only moves on commit. This is what lets
  // a body be longer than any one scratch buffer would allow — the old 1KB buffer
  // silently cut long footnotes mid-sentence before pagination ever saw them.
  char* captureBuffer() { return buffer ? buffer.get() + used : nullptr; }

  // Bytes of text a capture may hold, excluding the terminator commitCapture() writes.
  uint16_t captureCapacity() const {
    if (!buffer || used >= POOL_BYTES) return 0;
    const uint16_t room = static_cast<uint16_t>(POOL_BYTES - used - 1);
    return room < MAX_BODY_BYTES ? room : MAX_BODY_BYTES;
  }

  // Terminates and claims the first `len` captured bytes.
  // Returns the offset, or FOOTNOTE_POOL_NO_TEXT when the pool is full.
  uint16_t commitCapture(uint16_t len) {
    if (!buffer || len == 0 || len > captureCapacity()) return FOOTNOTE_POOL_NO_TEXT;
    const uint16_t offset = used;
    buffer[offset + len] = '\0';
    used = static_cast<uint16_t>(used + len + 1);
    return offset;
  }

  // Returns nullptr for FOOTNOTE_POOL_NO_TEXT or any out-of-range offset.
  const char* get(uint16_t offset) const {
    if (!buffer || offset >= used) return nullptr;
    return buffer.get() + offset;
  }

 private:
  std::unique_ptr<char[]> buffer;
  uint16_t used = 0;
};

// Anchor id → body text reference collected during pre-scan.
// `textOffset` indexes into the parser's FootnoteBodyPool, not this struct.
struct FootnoteBodyEntry {
  char id[64];
  uint16_t textOffset = FOOTNOTE_POOL_NO_TEXT;
  mutable int16_t cachedLineCount = -1;  // -1 = not yet computed
};
#include "../Page.h"
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  int preUntilDepth = INT_MAX;  // when set, we are inside a <pre> block (monospace + hard line breaks)
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  bool htmlEnded = false;          // </html> seen; a later expat error is trailing junk, not a broken chapter
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // The bottom margin makePages() already added to currentPageNextY for the
  // block it just finished, so the next block can collapse its own top margin
  // against it (CSS adjacent-sibling margin collapsing). Reset to 0 by anything
  // that is not a text block, so nothing collapses across an image or a rule.
  int16_t previousBlockBottomMargin = 0;
  int fontId;
  int codeFontId;  // monospace font id used for <pre> blocks (0 = no override)
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  // User spacing controls, handed to every ParsedText layout pass (body and table cells).
  TextLayoutOptions textLayout;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  bool footnoteDisplayOnPage;
  bool bionicReadingEnabled;
  std::string contentBase;
  std::string imageBasePath;

  // ── Inline images ──────────────────────────────────────────────────────────
  // A small image that shares its paragraph with text flows in the line (see
  // InlineImageToken.h); everything else stays a centred block image, as before.
  //
  // Images already added to currentTextBlock as tokens, waiting for the line they land on.
  // Bounded: at MAX_PENDING_INLINE_IMAGES the paragraph is soft-flushed, and an image that
  // still does not fit is placed as a block. Empty (no allocation) for books without any.
  struct PendingInlineImage {
    uint16_t id;
    std::unique_ptr<ImageBlock> block;
  };
  static constexpr size_t MAX_PENDING_INLINE_IMAGES = 16;
  std::vector<PendingInlineImage> pendingInlineImages;
  uint16_t nextInlineImageId = 1;
  // An inline-sized image that OPENS its paragraph cannot be classified when it is parsed:
  // followed by text it is inline (a drop-cap glyph, a leading icon); alone in its block it
  // is what it has always been, a centred block image (a scene-break ornament). It waits
  // here until the next word (inline) or the next block boundary (block) decides.
  struct LeadingImage {
    std::unique_ptr<ImageBlock> block;
    bool savedNextWordContinues = false;
  };
  LeadingImage leadingImage;
  // True once the block element that owns currentTextBlock's words has closed: the words are
  // still buffered (they are laid out when the next block starts), but an image that follows
  // is not inside that paragraph.
  bool paragraphClosed = false;
  bool addInlineImage(std::unique_ptr<ImageBlock> block, bool attachToPrevious);
  void placeBlockImage(std::unique_ptr<ImageBlock> block);
  void resolveLeadingImageAsBlock();
  void resolveLeadingImageAsInline();

  // ── Publisher decorations: bordered / shaded blocks ────────────────────────
  // One entry per open element that has a CSS border or a background shade. It records the
  // vertical extent of what is laid out while it is open (noteContent) and becomes a
  // PageBorderBox on each page the element touches. A fixed array, not a vector: nothing is
  // allocated per bordered element, and nesting deeper than this simply draws no frame.
  struct BoxScope {
    int16_t depth = 0;
    int16_t left = 0, right = 0, padTop = 0, padBottom = 0;
    CssBorderSide sides[4];
    bool shaded = false;
    bool continued = false;         // already drew a slice on an earlier page: top edge stays open
    int16_t top = -1, bottom = -1;  // content extent on the current page; -1 = nothing yet
  };
  static constexpr uint8_t MAX_BOX_SCOPES = 4;
  BoxScope boxScopes[MAX_BOX_SCOPES];
  uint8_t boxScopeCount = 0;
  void noteContent(int top, int bottom);
  void openBoxScope(BlockStyle& ownStyle, const CssStyle& cssStyle);
  void closeBoxScope();
  void emitBoxSegment(const BoxScope& box, bool closing);
  // Every page completion goes through here, so open boxes get their slice of the page.
  void completeCurrentPage();
  // CSS page-break-before/after: finish the paragraph in hand and start a fresh page, unless
  // the page is still empty.
  void forcePageBreak();
  // Style for an emptied text block that must not carry the spacing it has already spent.
  BlockStyle neutralBlockStyle() const;

  // ── Preformatted text (CSS white-space: pre / pre-wrap / pre-line) ─────────
  // Line breaks in the source are kept, and so is each line's leading indentation; runs of
  // spaces inside a line still collapse, so long lines wrap normally.
  bool preAtLineStart = false;            // inside a preformatted block, before a line's first glyph
  bool preLinesEmitted = false;           // a line of the current preformatted block has been laid out
  uint8_t prePendingBlankLines = 0;       // blank source lines waiting for the next line of text
  bool suppressParagraphSpacing = false;  // makePages(): this block is one line of a larger one
  void preLineBreak();
  void preLeadingSpace(int count);
  void beginPreLine();

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasUnderline = false, underline = false;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;

  // Streaming table grid (ported from witchhunt-reader). Only ONE row is buffered as ParsedText;
  // at </tr> it is laid out into TableRows packed into a PageTableFragment that never exceeds a
  // page. Anything the grid cannot represent degrades that row (or, for rowspan/nesting, the
  // rest of the table) to ordinary paragraphs, so nothing is ever dropped.
  struct BufferedTableCell {
    std::unique_ptr<ParsedText> text;
    bool isHeader = false;
    uint8_t colSpan = 1;
  };
  struct BufferedTableRow {
    std::vector<BufferedTableCell> cells;
    bool isHeaderRow = false;   // true when every cell in this row is <th>
    uint8_t effectiveCols = 0;  // sum of colSpan values: the row's real column footprint
  };
  struct LayoutRow {
    std::vector<TableCell> cells;
    uint16_t height = 0;  // content height + 2 x TABLE_CELL_PADDING
    bool isHeaderRow = false;
    uint8_t renderCols = 0;  // grid columns this row was laid out on
  };
  struct TableFragmentPacker {
    std::vector<TableRow> rows;
    uint16_t height = 0;
    uint8_t cols = 0;
    uint16_t totalWidth = 0;
    int16_t xInset = 0;  // left edge of the table box; non-zero when the <table> carries a left inset
    bool hasBorder = true;
  };
  struct BufferedTable {
    BufferedTableRow pendingRow;
    TableFragmentPacker packer;
    uint8_t columnCount = 0;
    uint16_t contentWidth = 0;
    int depth = 0;          // nesting depth; > 1 means we're inside a nested table
    bool hasBorder = true;  // false when border="0" on the <table> element
    bool degraded = false;  // whole rest of the table flows as paragraphs
    bool rowDegraded = false;
    bool rowOverflowed = false;
    size_t pendingRowBytes = 0;  // attributed bytes of the buffered row (see MAX_TABLE_ROW_BUFFER_BYTES)
    std::unique_ptr<BufferedTableRow> repeatHeader;  // first row, if all <th>: repeated on continuation pages
    uint16_t repeatHeaderHeight = 0;
    bool repeatHeaderResolved = false;
  };
  std::unique_ptr<BufferedTable> currentTable;
  BufferedTableCell* currentTableCell = nullptr;  // non-null while inside <td>/<th>

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  bool recordPageBreakAnchors = false;
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

  // First-body-element tracking: <a> links inside the first direct child of <body>
  // are treated as navigation (TOC), not footnote references.
  int bodyChildDepth = -1;  // depth of first direct child of <body>; -1 = not yet seen
  bool inFirstBodyElement = false;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  char currentFootnoteLinkText[24] = {};
  int currentFootnoteLinkTextLen = 0;
  char currentFootnoteLinkHref[64] = {};
  // References waiting for the page their word lands on. FootnoteRef, not
  // FootnoteEntry: neither list owns a body, and a list of full entries would have
  // carried a heap allocation each for text they never use.
  std::vector<std::pair<int, FootnoteRef>> pendingFootnotes;  // <wordIndex, ref>

  // Tail of a footnote body that did not fit on the page its anchor landed on; its
  // `text` points into footnoteBodyPool at the exact byte the next page must resume
  // from, so a body spanning several pages loses nothing at the seams. The pool is
  // allocated before the parse and released only after the last page is emitted, so
  // it outlives every deferral (never more than a handful of pages).
  std::vector<FootnoteRef> deferredFootnotes;
  int wordsExtractedInBlock = 0;

  // Pre-scanned anchor id → body text (heap-allocated, freed after main parse)
  static constexpr int MAX_FOOTNOTE_BODY_ENTRIES = 32;
  static constexpr int MAX_CROSS_FILES = 4;
  static constexpr int MAX_CROSS_FILE_NAME_LEN = 80;
  std::unique_ptr<FootnoteBodyEntry[]> footnoteBodyEntries;
  FootnoteBodyPool footnoteBodyPool;  // backing store for every entry's body text
  int footnoteBodyEntryCount = 0;
  const char* lookupFootnoteText(const char* href) const;
  int lookupFootnoteLineCount(const char* href, int width) const;
  static constexpr int MAX_TARGET_FRAGMENTS = 32;
  static int preScanAnchors(const std::string& filepath, FootnoteBodyEntry* entries, int maxEntries,
                            FootnoteBodyPool* pool, char (*crossFiles)[MAX_CROSS_FILE_NAME_LEN] = nullptr,
                            int* crossFileCount = nullptr, int maxCrossFiles = 0,
                            char (*collectFragments)[64] = nullptr, int* collectFragmentCount = nullptr,
                            int maxCollectFragments = 0, char (*filterFragments)[64] = nullptr,
                            int filterFragmentCount = 0);

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPartWordBuffer();
  // includeLastLine=false is a soft flush of an over-long paragraph: all but the last line
  // are emitted and the block stays open, so no bottom spacing or footnote fallback runs.
  void makePages(bool includeLastLine = true);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // Table grid
  bool heapAllowsTableRowLayout() const;
  bool ensurePage();
  void breakPage();
  void recordPendingAnchor();
  void attachPendingFootnotesToPage();
  int tableLineStep() const;
  std::unique_ptr<ParsedText> newCellText() const;
  void commitPendingRow();
  bool layoutTableRow(BufferedTableRow& bufRow, uint8_t columnCount, LayoutRow& out);
  void flushTableFragment(TableFragmentPacker& packer);
  void emitCellAsParagraph(BufferedTableCell& cell);
  void emitRowAsParagraphs(BufferedTableRow& row);
  void degradeRow(const char* reason);
  void degradeTable(const char* reason);
  void degradeRowAtOpenCell(const char* reason);
  void streamClosedCell(BufferedTableRow& row);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const int codeFontId, const float lineCompression,
                                 const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                 const uint16_t viewportWidth, const uint16_t viewportHeight,
                                 const bool hyphenationEnabled,
                                 const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
                                 const bool footnoteDisplayOnPage = true, const bool bionicReadingEnabled = false)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        codeFontId(codeFontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        footnoteDisplayOnPage(footnoteDisplayOnPage),
        bionicReadingEnabled(bionicReadingEnabled),
        contentBase(contentBase),
        imageBasePath(imageBasePath) {}

  ~ChapterHtmlSlimParser() = default;
  // Call before parseAndBuildPages(). A setter rather than one more constructor argument.
  void setTextLayout(const TextLayoutOptions& options) { textLayout = options; }
  bool parseAndBuildPages();
  void addLineToPage(std::unique_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
  // Also record the ids of page-break markers (epub:type="pagebreak"), which are
  // usually <span>s and so skipped as anchors. Only worth its RAM for a book that
  // has a publisher page list to jump by (see PageList.h); off by default.
  void setRecordPageBreakAnchors(const bool record) { recordPageBreakAnchors = record; }
};
