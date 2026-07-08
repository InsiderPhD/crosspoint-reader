#pragma once

#include <EpdFontFamily.h>

#include <string>

// A single laid-out word on a reader page, with the on-screen geometry needed to draw a
// selection cursor/highlight over it. Built transiently in EpubReaderActivity::startClipSelection
// from the live page layout and consumed by ClipSelectionActivity / ClipTextBuilder.
struct WordRef {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int pageIdx = 0;
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;
  bool endsWithInsertedHyphen = false;
};
