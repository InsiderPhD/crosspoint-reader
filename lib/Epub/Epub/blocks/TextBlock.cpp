#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstring>

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasBionic, const uint16_t textBytes) {
  // Layout documented in TextBlock.h: 16-bit arrays first, then 8-bit arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasBionic) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  if (bionicPresent) {
    bionicSuffixXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (bionicPresent) {
    bionicBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const BlockStyle& blockStyle,
                     const std::vector<uint8_t>& bionicBoundary, const std::vector<uint16_t>& bionicSuffixX)
    : blockStyle(blockStyle) {
  // Bionic annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasBionic = !bionicBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > 10000 ||
      (hasBionic && (words.size() != bionicBoundary.size() || words.size() != bionicSuffixX.size()))) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(bionicBoundary.size()),
            static_cast<uint32_t>(bionicSuffixX.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  bionicPresent = hasBionic;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical
  // row of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    bionicPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, bionicPresent, textBytes);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    bionicPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  // Pass 2: fill. Mutable aliases of the const views bound above.
  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (bionicPresent) {
    auto* suffixX = const_cast<uint16_t*>(bionicSuffixXArr);
    auto* boundary = const_cast<uint8_t*>(bionicBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      suffixX[i] = bionicSuffixX[i];
      boundary[i] = bionicBoundary[i];
    }
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  const int effectiveFontId = blockStyle.fontOverride != 0 ? blockStyle.fontOverride : fontId;
  const int ascender = renderer.getFontAscenderSize(effectiveFontId);
  for (uint16_t i = 0; i < numWords; i++) {
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const char* word = wordText(i);
    const uint16_t wordLen = wordTextLen(i);

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    const uint8_t boundary = bionicBoundary(i);
    if (boundary > 0) {
      // Bionic split: draw bold prefix then regular suffix using pre-computed pixel offset.
      // Max 9 codepoints = at most 36 UTF-8 bytes; stack buffer is safe.
      char boldBuf[40];
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordLen),
                                               sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      renderer.drawText(effectiveFontId, wordX, wordY, boldBuf, true, boldStyle);
      const int suffixX = wordX + static_cast<int>(bionicSuffixX(i));
      renderer.drawText(effectiveFontId, suffixX, wordY, word + boldLen, true, currentStyle);
    } else {
      renderer.drawText(effectiveFontId, wordX, wordY, word, true, currentStyle);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const int fullWordWidth = renderer.getTextWidth(effectiveFontId, word, currentStyle);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = wordY + ascender + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (wordLen >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visiblePtr = word + 3;
        const int prefixWidth = renderer.getTextAdvanceX(effectiveFontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(effectiveFontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      // SUP/SUB words are rendered at 50% glyph scale (see the baseline comment
      // above and drawText), but getTextWidth reports the full-size width, so the
      // underline would be drawn ~2x too long. Halve it to match the scaled glyphs.
      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim — its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob.
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>(bionicPresent ? 1 : 0));
  serialization::writePod(file, textBytes);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, bionicPresent, textBytes);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.fontOverride);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  uint8_t hasBionic;
  uint16_t textBytes;
  serialization::readPod(file, wc);
  serialization::readPod(file, hasBionic);
  serialization::readPod(file, textBytes);

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->bionicPresent = hasBionic != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->bionicPresent, textBytes);
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  // Style (alignment + margins/padding/indent)
  BlockStyle& blockStyle = block->blockStyle;
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.fontOverride);

  return block;
}
