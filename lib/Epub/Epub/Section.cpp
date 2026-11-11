#include "Section.h"

#include <Arduino.h>
#include <BufferedPrint.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <PrintSerialization.h>
#include <Serialization.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <deque>
#include <new>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "SpinePageIndex.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"
#include "parsers/VoidTagCloser.h"

namespace {
// Fork namespace. Every section.bin starts with this 4-byte magic (bytes 0xFF 'K' 'P' 'S'),
// then the uint8 version below. Upstream CrossPoint starts the file with a bare uint8
// version that never reaches 0xFF (its partial-build marker counts DOWN from 0xFE), and
// CrossInk starts with 0xFF 'C' 'X' 'S'; so a section.bin from either fork - or from a
// KatiePoint build before v50 - fails the magic check and is rebuilt instead of parsed,
// whatever its version number. Our files fail their checks the same way. Never change it.
constexpr uint32_t SECTION_CACHE_MAGIC = 0x53504BFF;  // bytes: 0xFF, "KPS"
// 41: TextBlock word data stored as one flat arena (offset table + NUL-terminated
//     text blob) instead of length-prefixed strings and per-field arrays.
// 44: tables render as PageTableFragment grids (new page element TAG_PageTable = 4);
//     also covers VoidTagCloser's layout change and hash-keyed shared img_ files.
// 45: a long paragraph's soft flush no longer restarts the paragraph: first-line indent
//     and top spacing apply once per paragraph (upstream d5693c321). Layout-only change.
// 46: user text-layout controls. Header gains characterSpacing, wordSpacingPercent and
//     paragraphIndent (cache validation); every TextBlock's style tail gains characterSpacing.
// 47: in a book with a publisher page list, the anchor map also carries the ids of
//     page-break markers (epub:type="pagebreak"), so Go to Page can land on them.
// 48: inline images (a small image that shares a paragraph with text is laid out in the
//     line; still a PageImage) and publisher decorations (new page element
//     TAG_PageBorderBox = 5; forced page breaks and white-space:pre change layout only).
//     Header gains layoutFeatures (see LAYOUT_FEATURES below).
// 49: content-based reading positions (upstream #2805): a fifth trailing header offset and a
//     uint32_t per page giving the visible-text offset where the page starts. Also stops
//     rendering <style>/<script>/<title>/<rp> found inside <body>.
// 50: header gains the SECTION_CACHE_MAGIC prefix (fork namespace); body unchanged.
// 51: layout-only rebuild for the upstream 2.0 layout fixes: text before a block/heading
//     start stays in its own paragraph (#4030), CSS font inherit (#4072), line breaks after
//     explicit hyphens/dashes (#3883), NBSP justification and carried NBSPs (#4142), and
//     SD-font advances snapped per glyph during layout (#3985). Binary layout unchanged.
// 52: 2.0 parity EPUB rendering: fieldset boxes, GIF images, line spacing as a percentage
//     of the font size, #id / descendant / child selectors, small caps (word style bit 64),
//     widow / orphan control, block font sizes on every board (the header's layout byte is
//     now TextLayoutOptions::layoutSignature()). Binary layout otherwise unchanged.
constexpr uint8_t SECTION_FILE_VERSION = 52;
// Staging buffer for page serialization, one per section build (not per page). 2KB is four
// SD sectors: a typical text page (~3-6KB serialized) goes out in two to four writes, and a
// field larger than the buffer (a long line's word arena) bypasses it. Heap, not stack: 2KB
// is far past the 256-byte limit for locals, on a task stack this build already runs deep in.
constexpr size_t PAGE_WRITE_BUFFER_SIZE = 2048;
// Layout features compiled into this build that change where text lands without changing any
// setting. Stored in the header and compared on load, so a card moved between boards with the
// same panel (X4 and X4C) rebuilds instead of showing the other board's layout.
// Computed per section from TextLayoutOptions::layoutSignature(): whether block font sizes
// were in use and which sizes (they are decided at layout time on the C3).
#if CROSSPOINT_EPUB_BLOCK_FONT_SIZES
// Under a heap-gated ladder (C3, tight heap) any cached layout is kept: rebuilding a chapter
// is the most memory-hungry thing the reader does, and both kinds draw correctly.
bool layoutSignatureAcceptable(const uint8_t fileSignature, const TextLayoutOptions& textLayout) {
  (void)fileSignature;
  return textLayout.fontSizesHeapGated;
}
#else
bool layoutSignatureAcceptable(uint8_t, const TextLayoutOptions&) { return false; }
#endif
constexpr uint32_t HEADER_SIZE =
    sizeof(uint32_t) + sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
    sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) +
    sizeof(uint8_t) + sizeof(bool) + sizeof(int8_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint8_t) +
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
// The header ends with the page count and five uint32_t offsets, patched once the build ends.
// Each offset is addressed from the end of the header, in this order.
constexpr uint32_t TRAILING_OFFSETS = 5;
constexpr uint32_t OFFSET_SLOT_LUT = HEADER_SIZE - sizeof(uint32_t) * 5;
constexpr uint32_t OFFSET_SLOT_ANCHORS = HEADER_SIZE - sizeof(uint32_t) * 4;
constexpr uint32_t OFFSET_SLOT_PARAGRAPHS = HEADER_SIZE - sizeof(uint32_t) * 3;
constexpr uint32_t OFFSET_SLOT_LIST_ITEMS = HEADER_SIZE - sizeof(uint32_t) * 2;
constexpr uint32_t OFFSET_SLOT_VISIBLE_TEXT = HEADER_SIZE - sizeof(uint32_t);
constexpr uint32_t PAGE_COUNT_POS = HEADER_SIZE - sizeof(uint32_t) * TRAILING_OFFSETS - sizeof(uint16_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t visibleTextOffset;
};
}  // namespace

uint32_t Section::onPageComplete(std::unique_ptr<Page> page, BufferedPrint& out) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  // Exact, because nothing is staged between pages: every exit below drains `out`.
  const uint32_t position = file.position();
  // A page is hundreds of 1-4 byte fields. Staged in RAM they reach the card as a few
  // PAGE_WRITE_BUFFER_SIZE writes instead of one storage-mutex take and SdFat call each;
  // the bytes and their order are unchanged.
  const bool serialized = page->serialize(out);
  // Drained even when serialize() failed: bytes left staged would be credited to the next
  // page's offset, and would be flushed into a file this build has by then closed.
  const bool flushed = out.flushBuffer();
  // failed() catches a short write that surfaced inside a serializer whose write result
  // is not checked (the plain writePod calls): the bytes were staged successfully and only
  // refused by the card at a later flush.
  if (!serialized || !flushed || out.failed()) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const uint8_t footnoteDisplay, const bool bionicReadingEnabled,
                                     const TextLayoutOptions& textLayout) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(fontId) +
                                   sizeof(lineCompression) + sizeof(extraParagraphSpacing) +
                                   sizeof(paragraphAlignment) + sizeof(viewportWidth) + sizeof(viewportHeight) +
                                   sizeof(pageCount) + sizeof(hyphenationEnabled) + sizeof(embeddedStyle) +
                                   sizeof(imageRendering) + sizeof(footnoteDisplay) + sizeof(bionicReadingEnabled) +
                                   sizeof(textLayout.characterSpacing) + sizeof(textLayout.wordSpacingPercent) +
                                   sizeof(textLayout.paragraphIndent) + sizeof(uint8_t) +
                                   sizeof(uint32_t) * TRAILING_OFFSETS,
                "Header size mismatch");
  serialization::writePod(file, SECTION_CACHE_MAGIC);
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, footnoteDisplay);
  serialization::writePod(file, bionicReadingEnabled);
  serialization::writePod(file, textLayout.characterSpacing);
  serialization::writePod(file, textLayout.wordSpacingPercent);
  serialization::writePod(file, textLayout.paragraphIndent);
  serialization::writePod(file, textLayout.layoutSignature());
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for visible-text LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const uint8_t footnoteDisplay,
                              const bool bionicReadingEnabled, const TextLayoutOptions& textLayout) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint32_t magic = 0;
    uint8_t version = 0;
    if (!serialization::tryReadPod(file, magic) || magic != SECTION_CACHE_MAGIC ||
        !serialization::tryReadPod(file, version) || version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: magic %08lx version %u", static_cast<unsigned long>(magic), version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    uint8_t fileFootnoteDisplay;
    bool fileBionicReadingEnabled;
    TextLayoutOptions fileTextLayout;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFootnoteDisplay);
    serialization::readPod(file, fileBionicReadingEnabled);
    serialization::readPod(file, fileTextLayout.characterSpacing);
    serialization::readPod(file, fileTextLayout.wordSpacingPercent);
    serialization::readPod(file, fileTextLayout.paragraphIndent);
    uint8_t fileLayoutFeatures;
    serialization::readPod(file, fileLayoutFeatures);

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || footnoteDisplay != fileFootnoteDisplay ||
        bionicReadingEnabled != fileBionicReadingEnabled ||
        textLayout.characterSpacing != fileTextLayout.characterSpacing ||
        textLayout.wordSpacingPercent != fileTextLayout.wordSpacingPercent ||
        textLayout.paragraphIndent != fileTextLayout.paragraphIndent ||
        (fileLayoutFeatures != textLayout.layoutSignature() &&
         !layoutSignatureAcceptable(fileLayoutFeatures, textLayout))) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  serialization::readPod(file, pageCount);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  // A cache built before the table existed gets its count recorded on first use; a no-op SD-wise
  // once the table already says so.
  SpinePageIndex::record(
      epub->getCachePath(),
      layoutKey(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                hyphenationEnabled, embeddedStyle, imageRendering, footnoteDisplay, bionicReadingEnabled, textLayout),
      static_cast<uint16_t>(epub->getSpineItemsCount()), static_cast<uint16_t>(spineIndex), pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

// Live build state (see Section.h). The parser holds a reference to tmpHtmlPath and a copy
// of a lambda that points at `sink`, so both live here, on the heap, for the whole build.
// Members are declared in construction order: the staged writer wraps the Section's own
// `file`, the sink refers to the writer and the table, and the parser comes last so it is
// destroyed first (its destructor closes the chapter stream it may still hold).
struct Section::BuildState {
  struct PageSink {
    Section& self;
    std::deque<PageLutEntry>& lut;
    BufferedPrint& out;
  };
  explicit BuildState(Section& owner)
      : fileOut(owner.file), pageOut(fileOut, PAGE_WRITE_BUFFER_SIZE), sink{owner, lut, pageOut} {}
  std::string tmpHtmlPath;
  std::string contentBase;
  std::string imageBasePath;
  uint32_t fileSize = 0;
  CssParser* cssParser = nullptr;
  // In-RAM page table, written to the file after the parse. A deque grows in ~512-byte nodes
  // (42 entries each) while the parser is fighting for heap, so a long chapter never needs one
  // large contiguous block: a vector doubling past 1024 pages needs 24 KB, and a failed
  // allocation aborts under -fno-exceptions.
  std::deque<PageLutEntry> lut;
  // Allocated once here, before the parser starts competing for heap, and released with the
  // build. BufferedPrint allocates nothrow and degrades to pass-through, so a refused
  // allocation costs only the speed-up. It sits on HalFilePrint, not on the file itself:
  // through a Print& a HalFile would be written one byte per call (see the adapter). The
  // header and the LUTs are written to `file` directly; that is safe because onPageComplete
  // leaves nothing staged.
  serialization::HalFilePrint fileOut;
  BufferedPrint pageOut;
  PageSink sink;
  std::unique_ptr<ChapterHtmlSlimParser> visitor;
  uint32_t startMs = 0;
  uint32_t layoutKey = 0;  // SpinePageIndex key of the settings this build is for
};

uint32_t Section::layoutKey(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                            const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                            const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                            const uint8_t imageRendering, const uint8_t footnoteDisplay,
                            const bool bionicReadingEnabled, const TextLayoutOptions& textLayout) {
  // FNV-1a over the header fields, in header order, plus the cache version so a format bump
  // never reads a stale table.
  uint32_t h = 2166136261u;
  const auto mix = [&h](const void* data, const size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
      h ^= p[i];
      h *= 16777619u;
    }
  };
  const uint8_t signature = textLayout.layoutSignature();
  mix(&SECTION_FILE_VERSION, sizeof(SECTION_FILE_VERSION));
  mix(&fontId, sizeof(fontId));
  mix(&lineCompression, sizeof(lineCompression));
  mix(&extraParagraphSpacing, sizeof(extraParagraphSpacing));
  mix(&paragraphAlignment, sizeof(paragraphAlignment));
  mix(&viewportWidth, sizeof(viewportWidth));
  mix(&viewportHeight, sizeof(viewportHeight));
  mix(&hyphenationEnabled, sizeof(hyphenationEnabled));
  mix(&embeddedStyle, sizeof(embeddedStyle));
  mix(&imageRendering, sizeof(imageRendering));
  mix(&footnoteDisplay, sizeof(footnoteDisplay));
  mix(&bionicReadingEnabled, sizeof(bionicReadingEnabled));
  mix(&textLayout.characterSpacing, sizeof(textLayout.characterSpacing));
  mix(&textLayout.wordSpacingPercent, sizeof(textLayout.wordSpacingPercent));
  mix(&textLayout.paragraphIndent, sizeof(textLayout.paragraphIndent));
  mix(&signature, sizeof(signature));
  return h;
}

std::string Section::anchorSpillPath() const {
  return epub->getCachePath() + "/.anchors_" + std::to_string(spineIndex) + ".tmp";
}

std::string Section::cacheFilePath(const std::string& bookCachePath, const int spineIndex) {
  return bookCachePath + "/sections/" + std::to_string(spineIndex / CACHE_BUCKET) + "/" + std::to_string(spineIndex) +
         ".bin";
}

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(cacheFilePath(epub->getCachePath(), spineIndex)) {}

Section::~Section() { abortSectionBuild(); }

bool Section::createSectionFile(const int fontId, const int codeFontId, const float lineCompression,
                                const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const uint8_t footnoteDisplay, const bool bionicReadingEnabled,
                                const TextLayoutOptions& textLayout, const std::function<void()>& popupFn) {
  if (!startSectionBuild(fontId, codeFontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, footnoteDisplay,
                         bionicReadingEnabled, textLayout, popupFn)) {
    return false;
  }
  return stepSectionBuild(0) == BuildStep::Done;
}

bool Section::startSectionBuild(const int fontId, const int codeFontId, const float lineCompression,
                                const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const uint8_t footnoteDisplay, const bool bionicReadingEnabled,
                                const TextLayoutOptions& textLayout, const std::function<void()>& popupFn) {
  abortSectionBuild();  // a build already in flight is the wrong one: it was asked for again
  pageCount = 0;
  const auto localPath = epub->getSpineItem(spineIndex).href;
  // The inflated (void-tag-repaired) chapter is kept beside the caches, keyed on the spine
  // alone: a rebuild after a font, margin or orientation change, and a retry of an abandoned
  // build, skip the multi-second inflate (the shape of witchhunt-reader's html cache). It is
  // produced as ".part" and renamed once complete, so its presence means it is whole; the
  // book's cache folder goes with the book, so it can never outlive the file it came from.
  const auto tmpHtmlPath = epub->getCachePath() + "/html_" + std::to_string(spineIndex) + ".bin";
  const auto partPath = tmpHtmlPath + ".part";

  // Create the cache directory and this spine's bucket if they don't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
    Storage.mkdir((sectionsDir + "/" + std::to_string(spineIndex / CACHE_BUCKET)).c_str());
  }

  // A build can run for many seconds with the render lock held, so the main
  // loop's idle rail collapse cannot fire. If the last refresh left the panel
  // powered, it would sit with VCOM/VGH/VGL applied for the whole build — the
  // classic left-powered drift. Drop the rails now; the next paint pays a PON.
  renderer.powerOffScreen();

  // Lend the inflate its 32KB dictionary out of the framebuffer for the duration
  // of the unzip, instead of asking the heap for a contiguous block it may no
  // longer be able to produce. See InflateScratchLease for the measurements.
  //
  // Safe here specifically because: (a) e-ink is bistable, so the panel keeps
  // showing the current page with no framebuffer backing it, and (b) the indexing
  // popup is not drawn until the parse starts, which is after this lease has
  // been released. Nothing renders between these braces.
  bool borrowedFramebuffer = false;
  bool success = false;
  uint32_t fileSize = 0;
  if (Storage.exists(tmpHtmlPath.c_str())) {
    FsFile kept;
    if (Storage.openFileForRead("SCT", tmpHtmlPath, kept)) {
      fileSize = kept.size();
      kept.close();
      success = fileSize > 0;
    }
    if (success) {
      LOG_DBG("SCT", "Reusing inflated chapter %s (%u bytes)", tmpHtmlPath.c_str(), static_cast<unsigned>(fileSize));
    }
  }
  if (!success) {
    InflateScratchLease scratch(renderer.getFrameBuffer(), renderer.getBufferSize());
    borrowedFramebuffer = scratch.active();
    if (borrowedFramebuffer) {
      LOG_DBG("SCT", "Unzipping with a framebuffer-leased dictionary (largest free block %u)",
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
    }

    // Retry logic for SD card timing issues
    for (int attempt = 0; attempt < 3 && !success; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(partPath.c_str())) {
        Storage.remove(partPath.c_str());
      }

      FsFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", partPath, tmpHtml)) {
        continue;
      }
      // Self-close HTML void elements on the way out: web-article exports ship
      // "<img ...>", which strict XML parsing rejects for the whole chapter.
      VoidTagCloser xhtmlOut(tmpHtml);
      success = epub->readItemContentsToStream(localPath, xhtmlOut, 1024) && xhtmlOut.finish();
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!success && Storage.exists(partPath.c_str())) {
        Storage.remove(partPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
      if (success && !Storage.rename(partPath.c_str(), tmpHtmlPath.c_str())) {
        LOG_ERR("SCT", "Could not keep the inflated chapter as %s", tmpHtmlPath.c_str());
        Storage.remove(partPath.c_str());
        success = false;
      }
    }

  }  // lease released: the framebuffer is ours to paint into again

  // The lease overwrote the framebuffer with dictionary bytes. Clear it so the
  // indexing popup below does not composite over garbage; the reader repaints the
  // page in full once the build finishes.
  if (borrowedFramebuffer) {
    renderer.clearScreen();
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  buildState_ = std::unique_ptr<BuildState>(new (std::nothrow) BuildState(*this));
  if (!buildState_) {
    LOG_ERR("SCT", "No room for the build state");
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  BuildState& st = *buildState_;
  st.tmpHtmlPath = tmpHtmlPath;
  st.fileSize = fileSize;
  st.startMs = millis();
  st.layoutKey =
      layoutKey(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                hyphenationEnabled, embeddedStyle, imageRendering, footnoteDisplay, bionicReadingEnabled, textLayout);

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    abortSectionBuild();
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, footnoteDisplay,
                         bionicReadingEnabled, textLayout);

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  st.contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  // Shared by every spine: images are keyed on their archive path, so one extraction serves all.
  st.imageBasePath = epub->getCachePath() + "/img_";

  if (embeddedStyle) {
    st.cssParser = epub->getCssParser();
    if (st.cssParser) {
      if (!st.cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }
  if (!st.pageOut.buffered()) {
    LOG_ERR("SCT", "Page write buffer alloc failed (%u bytes); writing pages unstaged",
            static_cast<unsigned>(PAGE_WRITE_BUFFER_SIZE));
  }

  // One captured pointer: more would outgrow std::function's two-pointer inline storage
  // and move the closure to the heap.
  BuildState::PageSink* const sink = &st.sink;
  st.visitor.reset(new (std::nothrow) ChapterHtmlSlimParser(
      epub, st.tmpHtmlPath, renderer, fontId, codeFontId, lineCompression, extraParagraphSpacing, paragraphAlignment,
      viewportWidth, viewportHeight, hyphenationEnabled,
      [sink](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
             const uint32_t visibleTextOffset) {
        sink->lut.push_back(
            {sink->self.onPageComplete(std::move(page), sink->out), paragraphIndex, listItemIndex, visibleTextOffset});
      },
      embeddedStyle, st.contentBase, st.imageBasePath, imageRendering, popupFn, st.cssParser, footnoteDisplay == 0,
      bionicReadingEnabled));
  if (!st.visitor) {
    LOG_ERR("SCT", "No room for the chapter parser");
    abortSectionBuild();
    return false;
  }
  st.visitor->setTextLayout(textLayout);
  // Anchors stream to the card as they are found (ChapterHtmlSlimParser::recordAnchor) and are
  // copied into the section file by finalize; removed with the build either way.
  st.visitor->setAnchorSpillPath(anchorSpillPath());
  // Go to Page lands on the ids of the book's page-break markers; record them
  // only for a book that actually ships a page list.
  st.visitor->setRecordPageBreakAnchors(Storage.exists(PageList::filePath(epub->getCachePath()).c_str()));
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  // TEMP diagnostic: fragmentation OOM suspected when cold-building a large chapter right after a
  // BookFusion download (synced progress jumps straight into a deep, big spine item). "largest" is
  // the real signal — free can read healthy while the largest contiguous block is too small for the
  // parse, aborting on a throwing new under -fno-exceptions. Remove once confirmed.
  LOG_DBG("SCT", "pre-parse heap spine=%d html=%d: free=%u largest=%u", spineIndex, fileSize, ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  if (!st.visitor->beginStreaming()) {
    LOG_ERR("SCT", "Failed to start the chapter parse");
    abortSectionBuild();
    return false;
  }
  return true;
}

Section::BuildStep Section::stepSectionBuild(const uint32_t budgetMs) {
  if (!buildState_) {
    LOG_ERR("SCT", "stepSectionBuild without a build");
    return BuildStep::Failed;
  }
  BuildState& st = *buildState_;
  const auto step = st.visitor->stepStreaming(budgetMs);
  if (step == ChapterHtmlSlimParser::StreamStep::More) {
    return BuildStep::More;
  }
  const bool parsed = step == ChapterHtmlSlimParser::StreamStep::Done && st.visitor->finishStreaming();
  // The inflated chapter stays for the next rebuild (see startSectionBuild).
  if (!parsed) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    abortSectionBuild();
    return BuildStep::Failed;
  }
  const bool finalized = finalizeSectionBuild();
  if (st.cssParser) {
    st.cssParser->clear();
  }
  if (finalized) {
    SpinePageIndex::record(epub->getCachePath(), st.layoutKey, static_cast<uint16_t>(epub->getSpineItemsCount()),
                           static_cast<uint16_t>(spineIndex), pageCount);
  }
  LOG_DBG("SCT", "Section %d built: %u pages in %lu ms", spineIndex, pageCount,
          static_cast<unsigned long>(millis() - st.startMs));
  buildState_.reset();
  if (!finalized) {
    pageCount = 0;
    return BuildStep::Failed;
  }
  return BuildStep::Done;
}

bool Section::finalizeSectionBuild() {
  BuildState& st = *buildState_;
  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : st.lut) {
    if (entry.fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, entry.fileOffset);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  serialization::writePod(file, st.visitor->anchorCount());
  if (st.visitor->anchorsSpilled()) {
    // The spill holds the records in the file's own format: append it verbatim.
    bool copied = st.visitor->finishAnchorSpill();
    FsFile spill;
    if (copied && !Storage.openFileForRead("SCT", anchorSpillPath(), spill)) copied = false;
    if (copied) {
      uint8_t chunk[256];  // the only stack buffer here; the writer's own staging is gone by now
      while (true) {
        const int n = spill.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        if (file.write(chunk, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
          copied = false;
          break;
        }
      }
      spill.close();
    }
    Storage.remove(anchorSpillPath().c_str());
    if (!copied) {
      LOG_ERR("SCT", "Failed to copy the anchor spill into the section file");
      file.close();
      Storage.remove(filePath.c_str());
      return false;
    }
  } else {
    for (const auto& [anchor, page] : st.visitor->getAnchors()) {
      serialization::writeString(file, anchor);
      serialization::writePod(file, page);
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(st.lut.size()));
  for (const auto& entry : st.lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : st.lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  // One uint32_t per page: the visible-text offset where the page starts (non-decreasing).
  const uint32_t visibleTextLutOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : st.lut) {
    serialization::writePod(file, entry.visibleTextOffset);
  }

  // Patch header with final pageCount and the five section offsets
  file.seek(PAGE_COUNT_POS);
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  serialization::writePod(file, visibleTextLutOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return true;
}

void Section::abortSectionBuild() {
  if (!buildState_) {
    return;
  }
  // The parser goes first: it may still hold the chapter stream and the anchor spill open.
  buildState_->visitor.reset();
  Storage.remove(anchorSpillPath().c_str());
  // `file` is the build's write handle. The header was never patched, so the file is
  // useless to loadSectionFile: remove it.
  if (file) {
    file.close();
  }
  Storage.remove(filePath.c_str());
  // The inflated chapter (complete by construction) is kept for the retry.
  if (buildState_->cssParser) {
    buildState_->cssParser->clear();
  }
  buildState_.reset();
  pageCount = 0;
}

int Section::activeBuildPercent() const {
  if (!buildState_ || !buildState_->visitor) {
    return 0;
  }
  return buildState_->visitor->streamPercent();
}

uint16_t Section::estimatedTotalPages() const {
  if (!buildState_) {
    return pageCount;
  }
  const int percent = activeBuildPercent();
  if (percent <= 0 || pageCount == 0) {
    return pageCount;
  }
  const uint32_t projected = static_cast<uint32_t>(pageCount) * 100U / static_cast<uint32_t>(percent);
  return static_cast<uint16_t>(std::min<uint32_t>(std::max<uint32_t>(projected, pageCount), 65535U));
}

std::optional<uint16_t> Section::activeBuildPageForAnchor(const std::string& anchor) const {
  if (!buildState_ || !buildState_->visitor) {
    return std::nullopt;
  }
  uint16_t page = 0;
  if (!buildState_->visitor->lookupAnchorInActiveBuild(anchor, page)) {
    return std::nullopt;
  }
  return page;
}

std::unique_ptr<Page> Section::loadPageFromActiveBuild(const uint16_t pageIndex) {
  if (!buildState_ || pageIndex >= pageCount || pageIndex >= buildState_->lut.size()) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: page %u out of range (built=%u)", pageIndex, pageCount);
    return nullptr;
  }
  const uint32_t offset = buildState_->lut[pageIndex].fileOffset;
  if (offset == 0) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: page %u was not written", pageIndex);
    return nullptr;
  }
  // The build writes pages through `file` without syncing per page, so its most recently
  // written sector and the directory entry's size may not be on the card yet. A separate read
  // handle only sees committed data, so flush the writer first.
  if (file) {
    file.flush();
  }
  FsFile readHandle;
  if (!Storage.openFileForRead("SCT", filePath, readHandle)) {
    return nullptr;
  }
  if (!readHandle.seek(offset)) {
    LOG_ERR("SCT", "loadPageFromActiveBuild: seek to %u failed", offset);
    readHandle.close();
    return nullptr;
  }
  auto page = Page::deserialize(readHandle);
  readHandle.close();
  return page;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(OFFSET_SLOT_LUT);
  uint32_t lutOffset;
  serialization::readPod(file, lutOffset);
  file.seek(lutOffset + sizeof(uint32_t) * currentPage);
  uint32_t pagePos;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(OFFSET_SLOT_ANCHORS);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

uint16_t Section::readCachedPageCount(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("SCT", path, f)) {
    return 0;
  }
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      version != SECTION_FILE_VERSION) {
    f.close();
    return 0;
  }
  // pageCount sits before the five trailing uint32_t offsets (LUT, anchor map, paragraph
  // LUT, list-item LUT, visible-text LUT) — the same PAGE_COUNT_POS createSectionFile patches.
  f.seek(PAGE_COUNT_POS);
  uint16_t pc = 0;
  f.read(&pc, sizeof(pc));
  f.close();
  return pc;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(OFFSET_SLOT_PARAGRAPHS);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(OFFSET_SLOT_PARAGRAPHS);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(OFFSET_SLOT_LIST_ITEMS);
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(OFFSET_SLOT_PARAGRAPHS);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  f.seek(PAGE_COUNT_POS);
  uint16_t count;
  serialization::readPod(f, count);
  if (page >= count) {
    return std::nullopt;
  }

  f.seek(OFFSET_SLOT_VISIBLE_TEXT);
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  const uint32_t entryOffset = visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t);
  if (visibleLutOffset < HEADER_SIZE || entryOffset + sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  f.seek(entryOffset);
  uint32_t result;
  serialization::readPod(f, result);
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  f.seek(PAGE_COUNT_POS);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  f.seek(OFFSET_SLOT_VISIBLE_TEXT);
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset < HEADER_SIZE || visibleLutOffset + static_cast<uint32_t>(count) * sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  if (!f.seek(visibleLutOffset)) {
    LOG_ERR("SCT", "Failed to seek visible-text LUT");
    return std::nullopt;
  }
  // The last page starting at or before `offset` holds it. Read in batches: one SD read per
  // 32 pages instead of one per page.
  uint16_t result = 0;
  constexpr uint32_t BATCH_PAGE_COUNT = 32;
  uint32_t starts[BATCH_PAGE_COUNT];  // 128-byte stack buffer; offsets stay in file order for ties.
  for (uint32_t base = 0; base < count; base += BATCH_PAGE_COUNT) {
    const uint32_t batch = std::min<uint32_t>(BATCH_PAGE_COUNT, count - base);
    const size_t bytes = batch * sizeof(uint32_t);
    if (f.read(starts, bytes) != static_cast<int>(bytes)) {
      LOG_ERR("SCT", "Failed to read visible-text LUT batch");
      return std::nullopt;
    }
    for (uint32_t i = 0; i < batch; ++i) {
      if (starts[i] > offset) return result;
      result = static_cast<uint16_t>(base + i);
      // Zero-width content (an image-only page) can give several pages the same start.
      if (preferFirstAtOffset && starts[i] == offset) return result;
    }
  }
  return result;
}
