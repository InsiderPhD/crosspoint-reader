// Host test for UrlUtils::resolveReference / origin. Run test/run_url_utils_test.sh.
#include <cstdio>
#include <string>

#include "src/util/UrlUtils.h"

static int failures = 0;

static void expectEq(const std::string& got, const std::string& want, const char* what) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got.c_str(), want.c_str());
    failures++;
  }
}

int main() {
  using UrlUtils::origin;
  using UrlUtils::resolveReference;
  const std::string base = "http://h:8080/opds/root.xml?page=2#top";

  expectEq(resolveReference(base, "fiction.xml"), "http://h:8080/opds/fiction.xml", "relative file");
  expectEq(resolveReference(base, "sub/x.xml?a=1"), "http://h:8080/opds/sub/x.xml?a=1", "relative with query");
  expectEq(resolveReference(base, "../up.xml"), "http://h:8080/up.xml", "dot-dot");
  expectEq(resolveReference(base, "./a/../b.xml"), "http://h:8080/opds/b.xml", "dot segments");
  expectEq(resolveReference(base, "/abs/x"), "http://h:8080/abs/x", "absolute path");
  expectEq(resolveReference(base, "?page=3"), "http://h:8080/opds/root.xml?page=3", "query only");
  expectEq(resolveReference(base, "//cdn.example/b.epub"), "http://cdn.example/b.epub", "network path");
  expectEq(resolveReference(base, "https://o.example/a"), "https://o.example/a", "absolute URL");
  expectEq(resolveReference("http://h", "a.xml"), "http://h/a.xml", "base without path");
  expectEq(resolveReference("http://h/opds/", "a.xml"), "http://h/opds/a.xml", "directory base");
  expectEq(resolveReference("h.example/opds/root", "a"), "http://h.example/opds/a", "base without scheme");
  expectEq(resolveReference("http://h/a/b", "../../../x"), "http://h/x", "dot-dot above root");

  expectEq(origin("HTTPS://User:pw@Example.COM:443/path?q"), "https://example.com", "default https port");
  expectEq(origin("http://example.com:80"), "http://example.com", "default http port");
  expectEq(origin("http://example.com:8080/x"), "http://example.com:8080", "explicit port");
  expectEq(origin("http://[::1]/x"), "http://[::1]", "ipv6 literal");
  expectEq(origin("example.com/x"), "", "no scheme");

  if (failures == 0) printf("UrlUtils: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
