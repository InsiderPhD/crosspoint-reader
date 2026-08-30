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
#include "../ParsedText.h"
#include "../blocks/ImageBlock.h"
#include "../blocks/TextBlock.h"
#include "../css/CssParser.h"
#include "../css/CssStyle.h"

class Page;
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
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  int fontId;
  int codeFontId;  // monospace font id used for <pre> blocks (0 = no override)
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  bool footnoteDisplayOnPage;
  bool bionicReadingEnabled;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

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
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
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
  void makePages();
  void emitHorizontalRule(const BlockStyle& blockStyle);
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
  bool parseAndBuildPages();
  void addLineToPage(std::unique_ptr<TextBlock> line);
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
};
