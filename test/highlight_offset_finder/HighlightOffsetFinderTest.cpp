// Content offsets the highlight finder reports must equal the ones the page
// parser records (raw Expat codepoints of <body> text outside head/style/
// script/title/rp), or a synced clip lands on the wrong words on another device.
// Expected values were cross-checked with Python's xml.parsers.expat.
#include <cstdio>
#include <memory>
#include <string>

#include "ChapterXPathResolver.h"
#include "HighlightOffsetFinder.h"

namespace {
int failures = 0;

constexpr char kFixture[] =
    "<?xml version=\"1.0\"?>\r\n<html><head><title>T</title><style>p{}</style></head>\r\n<body class=\"x\">\r\n"
    "<style>.a{color:red}</style>\r\n<p>Caf&#233; &amp; tea \xf0\x9f\x98\x80 here.</p>\r\n"
    "<script>var a = 1;</script>\r\n<div><span>The quick brown</span> fox <em>jumps</em> over</div>\r\n"
    "<p>Lone\rcarriage then target words</p>\r\n</body></html>";

void check(const char* clip, const uint32_t expStart, const uint32_t expEnd, const uint32_t expUtf16Start,
           const char* expXPath) {
  const Epub epub{std::string(kFixture)};
  uint32_t start = 0, end = 0, start16 = 0, end16 = 0;
  const auto result =
      HighlightOffsetFinder::find(epub, 0, clip, 0.5f, start, end, HighlightOffsetFinder::Unit::Codepoint);
  HighlightOffsetFinder::find(epub, 0, clip, 0.5f, start16, end16, HighlightOffsetFinder::Unit::Utf16);
  const std::string xpath =
      ChapterXPathResolver::findXPathForVisibleTextOffset(std::make_shared<Epub>(std::string(kFixture)), 0, start);
  if (result != HighlightOffsetFinder::Result::Found || start != expStart || end != expEnd ||
      start16 != expUtf16Start || xpath != expXPath) {
    failures++;
    std::printf("FAIL \"%s\": got %u-%u utf16 %u %s\n  expected %u-%u utf16 %u %s\n", clip, start, end, start16,
                xpath.c_str(), expStart, expEnd, expUtf16Start, expXPath);
  }
}
}  // namespace

int main() {
  check("Café & tea", 2, 12, 15, "/body/DocFragment[1]/body/p[1]/text()[1].0");
  // The emoji is one codepoint but two UTF-16 units.
  check("here.", 15, 20, 29, "/body/DocFragment[1]/body/p[1]/text()[1].13");
  check("quick brown fox jumps", 26, 47, 50, "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].4");
  // A lone CR is one LF to Expat.
  check("target words", 72, 84, 96, "/body/DocFragment[1]/body/p[2]/text()[1].19");
  if (failures == 0) std::printf("HighlightOffsetFinderTest: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
