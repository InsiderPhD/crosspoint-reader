#pragma once

#include <cstring>

struct FootnoteEntry {
  char number[24];
  char href[64];
  char text[128];  // Footnote body text (empty if not resolved)

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
    text[0] = '\0';
  }
};
