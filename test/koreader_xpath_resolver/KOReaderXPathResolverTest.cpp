// Host test for lib/KOReaderSync/ChapterXPathResolver (progress -> KOReader XPath).
// Run with test/run_koreader_xpath_resolver_test.sh.
#include <ChapterXPathResolver.h>

#include <cstdio>
#include <memory>
#include <string>

namespace {
int failures = 0;

void expectEq(const std::string& actual, const std::string& expected, const char* what) {
  if (actual == expected) {
    return;
  }
  failures++;
  std::printf("FAIL %s\n  expected: %s\n  actual:   %s\n", what, expected.c_str(), actual.c_str());
}

std::shared_ptr<Epub> epubWith(const char* xhtml) { return std::make_shared<Epub>(xhtml); }

constexpr char kParagraphFixture[] =
    "<html><head><title>ignored title</title></head><body><div><section>"
    "<p>First</p><p>Second paragraph</p>"
    "</section></div></body></html>";

// Calibre output from Kindle/MOBI sources may carry body text in <div>/<span> with no <p>
// beyond an empty filepos anchor. Visible offsets count every body codepoint, including the
// formatting whitespace between blocks:
//   0 "\n"   1 "\n"   2 "3"   3 " "   4 "\n"   5-13 "She drove"   14-26 " back to D.C."
//   27 "\n"  28-33 "“Who?”"   34 "\n"
constexpr char kKindleDivSpanFixture[] =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body class=\"calibre\">\n"
    "<p id=\"filepos1\" class=\"calibre1\"></p>\n"
    "<div class=\"calibre8\"><span class=\"calibre9\">3</span></div><div class=\"calibre10\"> </div>\n"
    "<div class=\"calibre12\"><span class=\"calibre6\"><span class=\"bold\">She drove</span> back to "
    "D.C.</span></div>\n"
    "<div class=\"calibre19\"><span class=\"calibre6\">\xE2\x80\x9CWho?\xE2\x80\x9D</span></div>\n"
    "</body></html>";

constexpr char kInlineStyleFixture[] =
    "<html><body><style>p { color: red; }</style><script>var x = 1;</script>"
    "<div>abcd</div></body></html>";
}  // namespace

int main() {
  // Text inside <p> keeps resolving as before.
  {
    const auto epub = epubWith(kParagraphFixture);
    // 21 visible codepoints; ceil(0.5 * 21) = 11 -> "Second paragraph" starts at 5 -> offset 6.
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f),
             "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[1].6", "paragraph text, 50%");
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f),
             "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[1].16", "paragraph text, 100%");
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.0f), "/body/DocFragment[1]/body",
             "paragraph text, 0%");
  }

  // Text outside <p>/<li> used to resolve to nothing (and the caller fell back to the empty <p>).
  {
    const auto epub = epubWith(kKindleDivSpanFixture);
    // Progress spans the text up to the last non-whitespace codepoint (34), so 1.0 lands at the
    // end of the final text node rather than in the trailing formatting newline.
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f),
             "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].6", "div/span text, 100%");
    // ceil(0.5 * 34) = 17 -> " back to D.C." starts at 14 -> offset 3 within the node.
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.5f),
             "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].3", "div/span text, 50%");
    // ceil(0.1 * 34) = 4: a formatting space. Whitespace is never an anchor, so the target is
    // carried to the start of the next text run (offset 0 -> the bare element path).
    expectEq(ChapterXPathResolver::findXPathForProgress(epub, 0, 0.1f),
             "/body/DocFragment[1]/body/div[3]/span[1]/span[1]", "div/span text, target in whitespace");
  }

  expectEq(ChapterXPathResolver::findXPathForProgress(
               epubWith("<html><body><div>not a paragraph or list item</div></body></html>"), 0, 0.5f),
           "/body/DocFragment[1]/body/div[1]/text()[1].14", "bare div");
  expectEq(ChapterXPathResolver::findXPathForProgress(epubWith("<html><body>loose text</body></html>"), 0, 1.0f),
           "/body/DocFragment[1]/body/text()[1].10", "text directly in body");

  // <style>/<script> bodies are not on the page and must not shift offsets.
  expectEq(ChapterXPathResolver::findXPathForProgress(epubWith(kInlineStyleFixture), 0, 0.5f),
           "/body/DocFragment[1]/body/div[1]/text()[1].2", "inline style and script skipped");

  expectEq(ChapterXPathResolver::findXPathForProgress(epubWith(""), 0, 0.5f), "", "empty chapter");
  expectEq(ChapterXPathResolver::findXPathForProgress(epubWith("<html><body><p>broken"), 0, 0.5f), "",
           "malformed chapter");
  expectEq(ChapterXPathResolver::findXPathForProgress(epubWith("<html><body>\n<p></p>\n</body></html>"), 0, 0.5f), "",
           "whitespace-only chapter");

  if (failures == 0) {
    std::printf("KOReaderXPathResolverTest: all checks passed\n");
    return 0;
  }
  return 1;
}
