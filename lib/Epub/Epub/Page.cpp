#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstring>

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

int Page::countWrappedLines(GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth) {
  if (!text || text[0] == '\0' || maxWidth <= 0) return 0;
  char lineBuf[120];
  char wordBuf[48];
  int lineLen = 0;
  int lines = 1;
  const char* p = text;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    int wLen = 0;
    const char* wStart = p;
    while (*p && *p != ' ') { p++; wLen++; }
    int wCopy = wLen < (int)sizeof(wordBuf) - 1 ? wLen : (int)sizeof(wordBuf) - 1;
    memcpy(wordBuf, wStart, wCopy);
    wordBuf[wCopy] = '\0';
    if (lineLen == 0) {
      memcpy(lineBuf, wordBuf, wCopy + 1);
      lineLen = wCopy;
    } else {
      const int savedLen = lineLen;
      const int space = (lineLen + 1 + wCopy < (int)sizeof(lineBuf)) ? 1 : 0;
      if (space) {
        lineBuf[lineLen] = ' ';
        memcpy(lineBuf + lineLen + 1, wordBuf, wCopy);
        lineBuf[lineLen + 1 + wCopy] = '\0';
        if (renderer.getTextWidth(fontId, lineBuf) > maxWidth) {
          // Doesn't fit: new line
          lineBuf[savedLen] = '\0';
          lines++;
          memcpy(lineBuf, wordBuf, wCopy + 1);
          lineLen = wCopy;
        } else {
          lineLen += 1 + wCopy;
        }
      } else {
        // Word can't fit in buffer alongside existing line: force new line
        lines++;
        memcpy(lineBuf, wordBuf, wCopy + 1);
        lineLen = wCopy;
      }
    }
  }
  return lines;
}

namespace {
void drawWrappedText(GfxRenderer& renderer, const int fontId, const char* text, const int x, int& y,
                     const int maxWidth, const int lineH) {
  char lineBuf[120];
  char wordBuf[48];
  int lineLen = 0;
  const char* p = text;
  while (true) {
    while (*p == ' ') p++;
    if (!*p) break;
    int wLen = 0;
    const char* wStart = p;
    while (*p && *p != ' ') { p++; wLen++; }
    int wCopy = wLen < (int)sizeof(wordBuf) - 1 ? wLen : (int)sizeof(wordBuf) - 1;
    memcpy(wordBuf, wStart, wCopy);
    wordBuf[wCopy] = '\0';
    if (lineLen == 0) {
      memcpy(lineBuf, wordBuf, wCopy + 1);
      lineLen = wCopy;
    } else {
      const int savedLen = lineLen;
      const int space = (lineLen + 1 + wCopy < (int)sizeof(lineBuf)) ? 1 : 0;
      if (space) {
        lineBuf[lineLen] = ' ';
        memcpy(lineBuf + lineLen + 1, wordBuf, wCopy);
        lineBuf[lineLen + 1 + wCopy] = '\0';
        if (renderer.getTextWidth(fontId, lineBuf) > maxWidth) {
          // Flush line without this word
          lineBuf[savedLen] = '\0';
          renderer.drawText(fontId, x, y, lineBuf);
          y += lineH;
          memcpy(lineBuf, wordBuf, wCopy + 1);
          lineLen = wCopy;
        } else {
          lineLen += 1 + wCopy;
        }
      } else {
        // Word + existing line overflow buffer: flush and start fresh
        renderer.drawText(fontId, x, y, lineBuf);
        y += lineH;
        memcpy(lineBuf, wordBuf, wCopy + 1);
        lineLen = wCopy;
      }
    }
  }
  if (lineLen > 0) {
    renderer.drawText(fontId, x, y, lineBuf);
    y += lineH;
  }
}
}  // namespace

void Page::renderFootnotes(GfxRenderer& renderer, const int fontId, const int xOffset,
                           const int viewportBottom, const int viewportWidth) const {
  const int lineH = renderer.getLineHeight(fontId);
  const int effectiveBottom = viewportBottom - lineH / 2;

  char buf[160];
  int totalLines = 0;
  for (const auto& fn : footnotes) {
    if (fn.text[0] == '\0') continue;
    snprintf(buf, sizeof(buf), "%s %s", fn.number, fn.text);
    totalLines += countWrappedLines(renderer, fontId, buf, viewportWidth);
  }
  if (totalLines == 0) return;

  const int ruleY = effectiveBottom - totalLines * lineH - 4;
  renderer.drawLine(xOffset, ruleY, xOffset + viewportWidth / 3, ruleY);

  int y = ruleY + 4;
  for (const auto& fn : footnotes) {
    if (fn.text[0] == '\0') continue;
    snprintf(buf, sizeof(buf), "%s %s", fn.number, fn.text);
    drawWrappedText(renderer, fontId, buf, xOffset, y, viewportWidth, lineH);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href) ||
        file.write(fn.text, sizeof(fn.text)) != sizeof(fn.text)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      page->elements.push_back(std::move(pi));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href) ||
        file.read(entry.text, sizeof(entry.text)) != sizeof(entry.text)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
    entry.text[sizeof(entry.text) - 1] = '\0';
  }

  return page;
}
