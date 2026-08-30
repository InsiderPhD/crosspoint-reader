#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) rather than
// five parallel vectors. A resident page holds ~25-30 of these blocks, and the
// vector-of-std::string layout cost ~250 small allocations per page load: that
// was the primary driver of heap fragmentation on the ESP32-C3, and it is what
// leaves the section builder unable to find its 32KB contiguous inflate window
// once the BLE stack is up. Ported from upstream crosspoint #2547.
//
// Arena layout, in order (2-byte alignment holds by construction: all 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[numWords]         byte offset of word i's text in text[]
//   int16_t  xpos[numWords]
//   uint16_t bionicSuffixX[numWords]   present only when bionicPresent
//   uint8_t  styles[numWords]
//   uint8_t  bionicBoundary[numWords]  present only when bionicPresent
//   char     text[textBytes]           all words back to back, NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to a C API (drawText) with no std::string materialization.
//
// Bionic split semantics (unchanged from the vector layout): boundary N > 0
// means the first N bytes of word i render bold, the remainder in the base
// style. N is bounded to 9 codepoints (<= 36 UTF-8 bytes) by the clamp in
// ParsedText::addWord. bionicSuffixX is the pre-computed pixel offset from the
// word start to the regular suffix. Both arrays are omitted from the arena
// entirely when no word on the line has a split (zero per-word RAM cost when
// bionic reading is disabled).
class TextBlock final : public Block {
 private:
  BlockStyle blockStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including NULs
  bool bionicPresent = false;
  bool isValid = true;
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (a bare new is not nothrow under -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint16_t* bionicSuffixXArr = nullptr;  // null when !bionicPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* bionicBoundaryArr = nullptr;  // null when !bionicPresent
  const char* textArr = nullptr;

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasBionic, uint16_t textBytes);
  void bindArenaPointers();

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. On arena OOM the block is empty and valid()
  // is false — callers must check and drop the line rather than use it.
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const BlockStyle& blockStyle = BlockStyle(),
                     const std::vector<uint8_t>& bionicBoundary = {},
                     const std::vector<uint16_t>& bionicSuffixX = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to a C API directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  uint8_t bionicBoundary(const uint16_t i) const { return bionicPresent ? bionicBoundaryArr[i] : 0; }
  uint16_t bionicSuffixX(const uint16_t i) const { return bionicPresent ? bionicSuffixXArr[i] : 0; }
  // True when layout inserted a visual hyphen at a line break for this word (not present in the
  // EPUB text). master does not yet track this per-word, so always false; clipping text export
  // simply keeps any trailing hyphen. Kept for API parity with the clipping text builder.
  bool wordEndsWithInsertedHyphen(size_t /*index*/) const { return false; }

  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
