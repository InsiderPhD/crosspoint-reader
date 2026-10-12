#pragma once
// Host-test stand-in for lib/Epub/Epub.h: one in-memory spine item.
#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

class Epub {
 public:
  struct SpineItem {
    std::string href;
  };
  explicit Epub(std::string xhtml) : xhtml(std::move(xhtml)) {}
  int getSpineItemsCount() const { return 1; }
  SpineItem getSpineItem(int) const { return {"chapter.xhtml"}; }
  bool readItemContentsToStream(const std::string&, Print& out, const size_t chunkSize) const {
    if (xhtml.empty()) return false;
    for (size_t pos = 0; pos < xhtml.size(); pos += chunkSize) {
      const size_t n = xhtml.size() - pos < chunkSize ? xhtml.size() - pos : chunkSize;
      out.write(reinterpret_cast<const uint8_t*>(xhtml.data()) + pos, n);
    }
    return true;
  }

 private:
  std::string xhtml;
};
