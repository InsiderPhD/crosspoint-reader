#include "HttpHeader.h"

namespace {
std::string trimAscii(std::string_view text) {
  const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  return std::string(text);
}
}  // namespace

void parseHttpHeaderLine(const std::string_view line, HttpHeader& header) {
  const size_t colon = line.find(':');
  header.name = trimAscii(line.substr(0, colon));
  if (colon == std::string_view::npos) {
    header.value.clear();
  } else {
    header.value = trimAscii(line.substr(colon + 1));
  }
}

std::string formatHttpHeaderLine(const HttpHeader& header, const bool maskValue) {
  if (header.name.empty()) return "";
  if (header.value.empty()) return header.name;
  return header.name + ": " + (maskValue ? "******" : header.value);
}
