// Host test for the plugin catalog paging rules (src/activities/plugins/
// PluginCatalogPaging.h). Cases follow upstream CrossPoint 2.0's
// test/plugin_catalog_paging. Run
// test/run_plugin_catalog_paging_test.sh.
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "PluginCatalogPaging.h"

using plugincatalogpaging::countsTowardPage;
using plugincatalogpaging::limitFor;
using plugincatalogpaging::PageWindow;
using plugincatalogpaging::Paging;
using plugincatalogpaging::pagingOf;
using Headers = std::vector<std::pair<std::string, std::string>>;

namespace {
int failures = 0;

void expect(const bool ok, const char* label) {
  if (!ok) {
    std::printf("FAIL %s\n", label);
    failures++;
  }
}

// Rows the server returned for one page, counted through a server window.
bool nextPageAfter(const Paging paging, const int rows, const int pageSize = 8) {
  PageWindow window(0, pageSize);
  for (int i = 0; i < rows; i++) window.take();
  return window.hasMore(paging);
}

// Rows the server returned for one page, `hidden` of them untitled (not shown).
bool nextPageWithHidden(const Paging paging, const int rows, const int hidden, const int pageSize = 8) {
  PageWindow window(0, pageSize);
  for (int i = 0; i < rows; i++) window.take(countsTowardPage(paging, i >= hidden));
  return window.hasMore(paging);
}

void testClassification() {
  const Headers none;
  expect(pagingOf("https://x/all.json", "", none) == Paging::Static, "static file");
  expect(pagingOf("https://x/all-{page}.json", "", none) == Paging::PageNumber, "page file");
  expect(pagingOf("https://x/e?perPage={limit}&page={page}", "", none) == Paging::SizedPages, "sized url");
  expect(pagingOf("https://x/s", "{\"page\":{page},\"per_page\":{limit}}", none) == Paging::SizedPages, "sized body");
  expect(pagingOf("https://x/b?offset={offset}&limit={limit}", "", none) == Paging::Offset, "offset");
  const Headers paged = {{"X-Page", "{page}"}};
  expect(pagingOf("https://x/b", "", paged) == Paging::PageNumber, "page header");
}

void testLimit() {
  expect(limitFor(Paging::SizedPages, 8) == 8, "sized limit");
  expect(limitFor(Paging::Offset, 8) == 9, "offset limit");
  expect(limitFor(Paging::PageNumber, 8) == 9, "page limit");
  expect(limitFor(Paging::Static, 8) == 9, "static limit");
}

void testWindows() {
  expect(nextPageAfter(Paging::SizedPages, 8), "sized full page continues");
  expect(!nextPageAfter(Paging::SizedPages, 7), "sized short page ends");
  expect(!nextPageAfter(Paging::SizedPages, 0), "sized empty page ends");
  for (const Paging paging : {Paging::Offset, Paging::PageNumber}) {
    expect(!nextPageAfter(paging, 8), "no lookahead row ends");
    expect(nextPageAfter(paging, 9), "lookahead row continues");
  }

  PageWindow second(8, 8);  // page 2 of a 20-row static file
  int shown = 0;
  for (int i = 0; i < 20; i++) shown += second.take() ? 1 : 0;
  expect(shown == 8, "static page 2 shows 8");
  expect(second.hasMore(Paging::Static), "static page 2 continues");
  PageWindow last(16, 8);  // page 3: 4 rows
  shown = 0;
  for (int i = 0; i < 20; i++) shown += last.take() ? 1 : 0;
  expect(shown == 4, "static page 3 shows 4");
  expect(!last.hasMore(Paging::Static), "static page 3 ends");

  expect(nextPageWithHidden(Paging::Offset, 9, 1), "hidden offset row counts");
  expect(nextPageWithHidden(Paging::PageNumber, 9, 1), "hidden page row counts");
  expect(nextPageWithHidden(Paging::SizedPages, 8, 1), "hidden sized row counts");
  expect(nextPageWithHidden(Paging::Offset, 9, 9), "all-untitled page continues");
  expect(!nextPageWithHidden(Paging::Offset, 8, 1), "short offset page ends");

  PageWindow first(0, 8);  // 9 rows, 1 untitled: 8 shown, nothing left over
  shown = 0;
  for (int i = 0; i < 9; i++) shown += first.take(countsTowardPage(Paging::Static, i != 3)) ? 1 : 0;
  expect(shown == 8, "static skips hidden row");
  expect(!first.hasMore(Paging::Static), "static hidden row is not a next page");
}

}  // namespace

int main() {
  testClassification();
  testLimit();
  testWindows();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("plugin catalog paging: all tests passed\n");
  return 0;
}
