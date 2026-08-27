// Host tests for htmlToPlainText(), used to render dictionary definitions that
// cannot be laid out as styled pages.
//
// Ported from upstream's gtest version to the self-contained assert style the
// rest of this project's host tests use (see release_json_parser), so it runs
// from test/run_html_to_plain_text_test.sh with no external dependency.

#include <cassert>
#include <cstdio>
#include <string>

#include "HtmlToPlainText.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_STR(actual, expected)                                                              \
  do {                                                                                            \
    const std::string _a = (actual);                                                              \
    const std::string _e = (expected);                                                            \
    if (_a != _e) {                                                                               \
      fprintf(stderr, "  FAIL: %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__,         \
              _a.c_str(), _e.c_str());                                                            \
      testsFailed++;                                                                              \
      return;                                                                                     \
    }                                                                                             \
  } while (0)

static void stripsTagsAndKeepsText() {
  ASSERT_STR(htmlToPlainText("<b>quixotic</b> <i>adj.</i>"), "quixotic adj.");
  ASSERT_STR(htmlToPlainText("<span class=\"x\">in span</span>"), "in span");
  ASSERT_STR(htmlToPlainText("plain"), "plain");
  ASSERT_STR(htmlToPlainText(""), "");
  testsPassed++;
}

static void blockElementsBecomeBreaks() {
  ASSERT_STR(htmlToPlainText("a<br>b"), "a\nb");
  ASSERT_STR(htmlToPlainText("<div>a</div><div>b</div>"), "a\nb");
  ASSERT_STR(htmlToPlainText("<p>a</p><p>b</p>"), "a\n\nb");
  // Consecutive breaks do not stack up.
  ASSERT_STR(htmlToPlainText("a<br><br><br>b"), "a\nb");
  testsPassed++;
}

static void headingsBreakLikeParagraphs() {
  // tagBreak() lists h1-h6 among the paragraph-breaking names, so they must
  // actually reach that comparison -- a name scan that stops at the first digit
  // reads "h1" as "h" and the heading runs into the text after it.
  for (const char* tag : {"h1", "h2", "h3", "h4", "h5", "h6"}) {
    const std::string html = std::string("<") + tag + ">title</" + tag + ">body";
    ASSERT_STR(htmlToPlainText(html), "title\n\nbody");
  }
  ASSERT_STR(htmlToPlainText("<hr>after"), "after");
  testsPassed++;
}

static void decodesEntities() {
  ASSERT_STR(htmlToPlainText("Tom &amp; Jerry"), "Tom & Jerry");
  ASSERT_STR(htmlToPlainText("a&nbsp;b"),
             "a\xC2\xA0"
             "b");
  ASSERT_STR(htmlToPlainText("&#65;&#66;"), "AB");
  ASSERT_STR(htmlToPlainText("&#x2014;"), "\xE2\x80\x94");
  // Not entities: left as written rather than swallowed.
  ASSERT_STR(htmlToPlainText("&notanentity;"), "&notanentity;");
  ASSERT_STR(htmlToPlainText("100% & up"), "100% & up");
  testsPassed++;
}

static void trimsSurroundingWhitespace() {
  ASSERT_STR(htmlToPlainText("<p>only</p>"), "only");
  ASSERT_STR(htmlToPlainText("text   "), "text");
  ASSERT_STR(htmlToPlainText("a\tb"), "a b");
  ASSERT_STR(htmlToPlainText("<br>a"), "a");
  testsPassed++;
}

static void survivesMalformedMarkup() {
  ASSERT_STR(htmlToPlainText("a <b"), "a <b");
  ASSERT_STR(htmlToPlainText("x < y"), "x < y");
  ASSERT_STR(htmlToPlainText("<!-- comment -->kept"), "kept");
  testsPassed++;
}

int main() {
  stripsTagsAndKeepsText();
  blockElementsBecomeBreaks();
  headingsBreakLikeParagraphs();
  decodesEntities();
  trimsSurroundingWhitespace();
  survivesMalformedMarkup();

  printf("htmlToPlainText: %d passed, %d failed\n", testsPassed, testsFailed);
  return testsFailed == 0 ? 0 : 1;
}
