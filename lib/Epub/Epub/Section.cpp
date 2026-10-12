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

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"
#include "parsers/VoidTagCloser.h"

namespace {
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
constexpr uint8_t SECTION_FILE_VERSION = 47;
// Staging buffer for page serialization, one per section build (not per page). 2KB is four
// SD sectors: a typical text page (~3-6KB serialized) goes out in two to four writes, and a
// field larger than the buffer (a long line's word arena) bypasses it. Heap, not stack: 2KB
// is far past the 256-byte limit for locals, on a task stack this build already runs deep in.
constexpr size_t PAGE_WRITE_BUFFER_SIZE = 2048;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(uint8_t) + sizeof(bool) + sizeof(int8_t) + sizeof(uint8_t) +
                                 sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t);

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
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
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(footnoteDisplay) +
                                   sizeof(bionicReadingEnabled) + sizeof(textLayout.characterSpacing) +
                                   sizeof(textLayout.wordSpacingPercent) + sizeof(textLayout.paragraphIndent) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
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
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
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
    uint8_t version;
    serialization::readPod(file, version);
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
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

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || footnoteDisplay != fileFootnoteDisplay ||
        bionicReadingEnabled != fileBionicReadingEnabled ||
        textLayout.characterSpacing != fileTextLayout.characterSpacing ||
        textLayout.wordSpacingPercent != fileTextLayout.wordSpacingPercent ||
        textLayout.paragraphIndent != fileTextLayout.paragraphIndent) {
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

bool Section::createSectionFile(const int fontId, const int codeFontId, const float lineCompression,
                                const bool extraParagraphSpacing, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const uint8_t footnoteDisplay, const bool bionicReadingEnabled,
                                const TextLayoutOptions& textLayout, const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
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
  // popup is not drawn until parseAndBuildPages(), which is after this lease has
  // been released. Nothing renders between these braces.
  bool borrowedFramebuffer = false;
  bool success = false;
  uint32_t fileSize = 0;
  {
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
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      FsFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
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
      if (!success && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
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

  if (!Storage.openFileForWrite("SCT", filePath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, footnoteDisplay,
                         bionicReadingEnabled, textLayout);
  // One up-front block instead of ~log2(pages) grow-copy-free cycles while the
  // parser is fighting for contiguous heap. A page is ~2-3KB of XHTML, so
  // 2KB/page slightly over-estimates; the cap bounds the block at 4KB (512 x 8B)
  // and anything longer just grows past it as before.
  constexpr uint32_t HTML_BYTES_PER_PAGE_ESTIMATE = 2048;
  constexpr uint32_t MAX_LUT_RESERVE = 512;
  std::vector<PageLutEntry> lut;
  lut.reserve(std::min<uint32_t>(fileSize / HTML_BYTES_PER_PAGE_ESTIMATE + 1, MAX_LUT_RESERVE));

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  // Shared by every spine: images are keyed on their archive path, so one extraction serves all.
  std::string imageBasePath = epub->getCachePath() + "/img_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Allocated once here, before the parser starts competing for heap, and released when
  // this function returns. BufferedPrint allocates nothrow and degrades to pass-through, so
  // a refused allocation costs only the speed-up. It sits on HalFilePrint, not on the file
  // itself: through a Print& a HalFile would be written one byte per call (see the adapter).
  // The header above and the LUTs below are written to `file` directly; that is safe because
  // onPageComplete leaves nothing staged.
  serialization::HalFilePrint fileOut(file);
  BufferedPrint pageOut(fileOut, PAGE_WRITE_BUFFER_SIZE);
  if (!pageOut.buffered()) {
    LOG_ERR("SCT", "Page write buffer alloc failed (%u bytes); writing pages unstaged",
            static_cast<unsigned>(PAGE_WRITE_BUFFER_SIZE));
  }
  // One captured reference: three would outgrow std::function's two-pointer inline storage
  // and move the closure to the heap.
  struct PageSink {
    Section& self;
    std::vector<PageLutEntry>& lut;
    BufferedPrint& out;
  } pageSink{*this, lut, pageOut};

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, codeFontId, lineCompression, extraParagraphSpacing, paragraphAlignment,
      viewportWidth, viewportHeight, hyphenationEnabled,
      [&pageSink](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        pageSink.lut.push_back(
            {pageSink.self.onPageComplete(std::move(page), pageSink.out), paragraphIndex, listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser, footnoteDisplay == 0,
      bionicReadingEnabled);
  visitor.setTextLayout(textLayout);
  // Go to Page lands on the ids of the book's page-break markers; record them
  // only for a book that actually ships a page list.
  visitor.setRecordPageBreakAnchors(Storage.exists(PageList::filePath(epub->getCachePath()).c_str()));
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  // TEMP diagnostic: fragmentation OOM suspected when cold-building a large chapter right after a
  // BookFusion download (synced progress jumps straight into a deep, big spine item). "largest" is
  // the real signal — free can read healthy while the largest contiguous block is too small for the
  // parse, aborting on a throwing new under -fno-exceptions. Remove once confirmed.
  LOG_DBG("SCT", "pre-parse heap spine=%d html=%d: free=%u largest=%u", spineIndex, fileSize, ESP.getFreeHeap(),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (const auto& entry : lut) {
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
  const auto& anchors = visitor.getAnchors();
  serialization::writePod(file, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
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
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
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
  uint8_t version = 0;
  if (f.read(&version, sizeof(version)) != sizeof(version) || version != SECTION_FILE_VERSION) {
    f.close();
    return 0;
  }
  // pageCount sits before the four trailing uint32_t offsets (LUT, anchor map,
  // paragraph LUT, list-item LUT) — must match the patch layout in
  // createSectionFile (seek at HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount)).
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t));
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
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
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
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
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
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
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
