#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <vector>

#include "OpdsJsonParser.h"
#include "OpdsParser.h"

/**
 * A catalog response of either kind: sniffs the first byte that is not a BOM
 * or whitespace ('<' Atom/XML, '{' OPDS 2 JSON) and only then allocates the
 * matching parser, so at most one exists. Settings given beforehand
 * (description capture, language) are handed to it on creation.
 */
class OpdsFeedParser final : public Print {
 public:
  void setPreferredLanguage(const char* lang);
  void captureDescriptionFor(const std::string& href, size_t maxBytes);

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override;
  void flush() override;

  bool error() const;
  explicit operator bool() const { return !error(); }
  bool isJson() const { return json != nullptr; }

  std::vector<OpdsEntry> takeEntries();
  std::string takeDescription();
  const std::string& getSearchTemplate() const;
  const std::string& getNextPageUrl() const;
  const std::string& getPrevPageUrl() const;

 private:
  bool start(uint8_t firstByte);

  std::unique_ptr<OpdsParser> xml;
  std::unique_ptr<OpdsJsonParser> json;
  bool failed = false;
  uint8_t bomBytes = 0;
  char language[12] = {};
  std::string descriptionHref;
  size_t descriptionMax = 0;
};
