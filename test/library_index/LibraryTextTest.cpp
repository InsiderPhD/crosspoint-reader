// Host test for the Library index text normalisation and CLX1 layout maths.
// Plain checks, no framework: see test/run_library_index_test.sh.

#include <cstdio>
#include <cstring>
#include <string>

#include "LibraryFormat.h"
#include "LibraryText.h"

namespace {
int failures = 0;

void expectEq(const std::string& got, const std::string& want, const char* what) {
  if (got == want) return;
  std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
  failures++;
}

void expectTrue(const bool ok, const char* what) {
  if (ok) return;
  std::printf("FAIL %s\n", what);
  failures++;
}
}  // namespace

int main() {
  using namespace library;

  // Both Unicode normal forms must fold alike: a card written by macOS holds
  // decomposed names, one written by Windows or Calibre precomposed ones.
  expectEq(fold("pand\xC3\xA9mie"), "pandemie", "fold NFC");
  expectEq(fold("pande\xCC\x81mie"), "pandemie", "fold NFD");
  expectEq(fold("Vi\xE1\xBB\x87t Nam"), "viet nam", "fold doubly accented");
  expectEq(fold("S\xC3\xB8ren"), "soren", "fold letter with no decomposition");
  expectEq(fold("Stra\xC3\x9F"
                "e"),
           "strasse", "fold sharp s");
  expectEq(fold("  The  Lord -- of the Rings!  "), "the lord of the rings", "fold collapses punctuation");
  expectEq(fold("O\xE2\x80\x99"
                "Brien"),
           "o'brien", "fold keeps apostrophes");
  expectEq(fold("\xD0\x92\xD0\xBE\xD0\xB9\xD0\xBD\xD0\xB0"), "\xD0\x92\xD0\xBE\xD0\xB9\xD0\xBD\xD0\xB0",
           "fold keeps non-Latin letters");
  expectEq(fold(std::string_view("ab\xC3", 3)), "ab", "fold stops at a truncated sequence");

  expectEq(cleanPersonName("Austen, Jane"), "Jane Austen", "single comma is turned round");
  expectEq(cleanPersonName("Smith, John, Jr."), "Smith, John, Jr", "multi comma is left alone");
  expectEq(cleanPersonName("George Sand [Sand, George]"), "George Sand", "bracketed span dropped");
  expectEq(cleanPersonName("Neil Gaiman & Terry Pratchett"), "Neil Gaiman & Terry Pratchett",
           "ampersand co-authors survive for the Authors browser");
  expectEq(cleanPersonName("First Author; Second Author"), "First Author", "semicolon cuts secondary authors");

  expectEq(authorKey("Lu, Xun"), authorKey("Xun Lu"), "author key ignores name order");
  expectEq(authorKey("Herbert G Wells"), authorKey("Herbert Wells"), "author key ignores initials");
  expectTrue(authorKey("Wollstonecraft, Mary").size() <= AUTHOR_KEY_MAX_BYTES, "author key is bounded");

  expectEq(surnameKey("Herman Melville"), "melville herman", "surname key");
  expectEq(surnameKey("Plato"), "plato", "single-word surname key");

  expectTrue(matchesQuery("wuthering heights", "wut hei"), "query words prefix book words");
  expectTrue(!matchesQuery("wuthering heights", "eights"), "query does not match mid-word");
  expectTrue(matchesQuery("l'eneide", "eneide"), "query reaches past an apostrophe");

  expectEq(joinLibraryPath("/", "a.epub"), "/a.epub", "root join has one separator");
  expectEq(joinLibraryPath("/books", "a.epub"), "/books/a.epub", "folder join");

  // Layout: every section starts on a sector, and a header only validates
  // against the exact file size it describes.
  ClixHeader h{};
  std::memcpy(h.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  h.formatVersion = CLIX_FORMAT_VERSION;
  h.foldVersion = CLIX_FOLD_VERSION;
  h.bookCount = 300;
  h.folderCount = 4;
  layoutSections(h, 97, 12345);
  expectTrue(h.folderStart % CLIX_ALIGN == 0 && h.recordStart % CLIX_ALIGN == 0 && h.permStart % CLIX_ALIGN == 0 &&
                 h.nameStart % CLIX_ALIGN == 0,
             "sections are sector aligned");
  expectTrue(recordOffset(h, 2) == h.recordStart + 2 * sizeof(ClixRecord), "record stride");
  expectTrue(validateHeader(h, h.selfSize) == ClixValidity::Ok, "consistent header validates");
  expectTrue(validateHeader(h, h.selfSize - 1) == ClixValidity::SizeMismatch, "truncated file is rejected");
  ClixHeader stale = h;
  stale.foldVersion = CLIX_FOLD_VERSION - 1;
  expectTrue(validateHeader(stale, stale.selfSize) == ClixValidity::StaleFoldVersion, "stale fold is reported");
  ClixHeader bad = h;
  bad.nameStart += CLIX_ALIGN;
  expectTrue(validateHeader(bad, bad.selfSize) == ClixValidity::SectionsInconsistent, "moved section is rejected");
  expectTrue(clixPathHash("/a.epub", 7) != clixPathHash("/b.epub", 7), "path hash discriminates");

  if (failures == 0) std::printf("library index text/format: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
