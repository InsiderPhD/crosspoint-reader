#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "TextLayoutOptions.h"

class BufferedPrint;
class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;

  void writeSectionFileHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                              uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                              bool embeddedStyle, uint8_t imageRendering, uint8_t footnoteDisplay,
                              bool bionicReadingEnabled, const TextLayoutOptions& textLayout);
  // `out` is the build's staged view of `file` (see createSectionFile). It is empty on
  // entry and on return, so file.position() is exact at every page boundary.
  uint32_t onPageComplete(std::unique_ptr<Page> page, BufferedPrint& out);

  // Live state of a section build: the page table, the staged writer and the parser, which
  // must survive between stepSectionBuild() calls. Defined in Section.cpp so the parser type
  // stays out of this header; heap-owned so the parser's pointer to its page sink stays put.
  struct BuildState;
  std::unique_ptr<BuildState> buildState_;
  // Where a build streams the chapter's anchor map while it runs (see finalizeSectionBuild).
  std::string anchorSpillPath() const;
  // Writes the page LUT, anchor map and per-page tables and patches the header, then closes
  // the file. The build state's tables are consumed. False leaves no cache file behind.
  bool finalizeSectionBuild();

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Both out of line: the unique_ptr<BuildState> member needs the complete type wherever a
  // Section is constructed or destroyed, and a Section that dies mid-build must take its
  // half-written cache file with it.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, uint8_t footnoteDisplay, bool bionicReadingEnabled,
                       const TextLayoutOptions& textLayout);
  bool clearCache() const;
  bool createSectionFile(int fontId, int codeFontId, float lineCompression, bool extraParagraphSpacing,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, uint8_t footnoteDisplay,
                         bool bionicReadingEnabled, const TextLayoutOptions& textLayout,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  // ── Build-while-you-read ──────────────────────────────────────────────────
  // The same build as createSectionFile, in slices, so the reader can draw the pages already
  // laid out while the rest of the chapter is still being indexed (the shape of
  // witchhunt-reader's Background-C). startSectionBuild inflates the chapter to a temp file
  // (lending the framebuffer to the inflate, as the blocking build does), opens the cache file
  // and the parser; stepSectionBuild parses for about budgetMs (0 = to the end) and returns
  // More while the chapter has more to give. Done means the cache file is complete and
  // loadPageFromSectionFile works; Failed means it was removed. Everything runs on whichever
  // task calls it -- the caller serialises these against the render task.
  enum class BuildStep : uint8_t { More, Done, Failed };
  bool startSectionBuild(int fontId, int codeFontId, float lineCompression, bool extraParagraphSpacing,
                         uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                         bool hyphenationEnabled, bool embeddedStyle, uint8_t imageRendering, uint8_t footnoteDisplay,
                         bool bionicReadingEnabled, const TextLayoutOptions& textLayout,
                         const std::function<void()>& popupFn = nullptr);
  BuildStep stepSectionBuild(uint32_t budgetMs);
  bool hasActiveBuild() const { return static_cast<bool>(buildState_); }
  // Pages fully written so far; pages [0, activeBuildPageCount()) can be read with
  // loadPageFromActiveBuild(). Equals pageCount once the build is done.
  uint16_t activeBuildPageCount() const { return pageCount; }
  // Percent of the chapter consumed (0-100); 0 when no build is live.
  int activeBuildPercent() const;
  // The page count the chapter is heading for: pages so far scaled by the consumed fraction,
  // never below what is built. pageCount when no build is live.
  uint16_t estimatedTotalPages() const;
  // Where an anchor landed in the pages laid out so far (the answer the finished cache gives).
  std::optional<uint16_t> activeBuildPageForAnchor(const std::string& anchor) const;
  // Reads a page the build has written, through a second handle on the file the build is
  // still writing (the writer is flushed first). Call between slices, never during one.
  std::unique_ptr<Page> loadPageFromActiveBuild(uint16_t pageIndex);
  // Drops an in-flight build: the half-written cache file (its header was never patched) and
  // the inflated temp file go, the parser is freed. No-op without a build.
  void abortSectionBuild();

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Where spine `spineIndex` of the book cached at `bookCachePath` keeps its layout:
  // "<cache>/sections/<spineIndex / CACHE_BUCKET>/<spineIndex>.bin". A FAT open walks its
  // folder from the start, so a book of a thousand files in one folder paid hundreds of block
  // reads to open one chapter; buckets of CACHE_BUCKET keep every walk short (the shape of
  // witchhunt-reader's spineCacheDir). "sections" stays the top folder, so every "re-index"
  // that removes it is unchanged.
  static constexpr int CACHE_BUCKET = 32;
  static std::string cacheFilePath(const std::string& bookCachePath, int spineIndex);
  // One number for the render settings a section is laid out under (the same ones the cache
  // header stores and loadSectionFile compares): what SpinePageIndex keys its counts on.
  static uint32_t layoutKey(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                            uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                            bool embeddedStyle, uint8_t imageRendering, uint8_t footnoteDisplay,
                            bool bionicReadingEnabled, const TextLayoutOptions& textLayout);
  // Read only the page count from a cached section file header without loading the full section.
  // Returns 0 if the file doesn't exist, is corrupt, or has a mismatched version.
  static uint16_t readCachedPageCount(const std::string& path);

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Exact zero-based visible Unicode-codepoint offset (see ChapterHtmlSlimParser::
  // visibleTextOffset) where a rendered page starts, from the finished section cache.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;

  // The page containing an exact visible-text offset: the last page starting at or before it.
  // When preferFirstAtOffset is true, ties caused by zero-width content such as an image-only
  // page select the first page at that offset.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;
};
