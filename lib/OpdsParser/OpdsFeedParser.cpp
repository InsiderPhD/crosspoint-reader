#include "OpdsFeedParser.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

void OpdsFeedParser::setPreferredLanguage(const char* lang) {
  const size_t len = lang ? strnlen(lang, sizeof(language) - 1) : 0;
  if (len) memcpy(language, lang, len);
  language[len] = '\0';
}

void OpdsFeedParser::captureDescriptionFor(const std::string& href, const size_t maxBytes) {
  descriptionHref = href;
  descriptionMax = maxBytes;
}

bool OpdsFeedParser::start(const uint8_t firstByte) {
  if (firstByte == '<') {
    xml = makeUniqueNoThrow<OpdsParser>();
    if (xml && descriptionMax) xml->captureDescriptionFor(descriptionHref, descriptionMax);
  } else if (firstByte == '{') {
    json = makeUniqueNoThrow<OpdsJsonParser>();
    if (json) {
      json->setPreferredLanguage(language);
      if (descriptionMax) json->captureDescriptionFor(descriptionHref, descriptionMax);
    }
  } else {
    LOG_ERR("OPDS", "Feed is neither XML nor JSON");
    return false;
  }
  if (!xml && !json) {
    LOG_ERR("OPDS", "Cannot allocate the feed parser");
    return false;
  }
  return true;
}

size_t OpdsFeedParser::write(const uint8_t* bytes, const size_t length) {
  if (failed || (!bytes && length)) return 0;
  size_t start = 0;
  while (!xml && !json && start < length) {
    const uint8_t b = bytes[start];
    if (bomBytes) {  // inside a UTF-8 BOM: EF BB BF
      if (b != (bomBytes == 1 ? 0xBB : 0xBF)) {
        failed = true;
        return 0;
      }
      bomBytes = bomBytes == 1 ? 2 : 0;
      ++start;
    } else if (b == 0xEF) {
      bomBytes = 1;
      ++start;
    } else if (b == ' ' || b == '\r' || b == '\n' || b == '\t') {
      ++start;
    } else if (!this->start(b)) {
      failed = true;
      return 0;
    }
  }
  if (!xml && !json) return length;  // only whitespace so far
  Print& parser = xml ? static_cast<Print&>(*xml) : static_cast<Print&>(*json);
  const size_t rest = length - start;
  return rest == 0 || parser.write(bytes + start, rest) == rest ? length : 0;
}

void OpdsFeedParser::flush() {
  if (xml) {
    xml->flush();
  } else if (json) {
    json->flush();
  } else {
    failed = true;  // empty body
  }
}

bool OpdsFeedParser::error() const {
  if (failed) return true;
  if (xml) return xml->error();
  if (json) return json->error();
  return false;
}

std::vector<OpdsEntry> OpdsFeedParser::takeEntries() {
  if (xml) return std::move(*xml).getEntries();
  if (json) return std::move(*json).getEntries();
  return {};
}

std::string OpdsFeedParser::takeDescription() {
  if (xml) return xml->takeDescription();
  if (json) return json->takeDescription();
  return {};
}

const std::string& OpdsFeedParser::getSearchTemplate() const {
  static const std::string empty;
  return xml ? xml->getSearchTemplate() : json ? json->getSearchTemplate() : empty;
}

const std::string& OpdsFeedParser::getNextPageUrl() const {
  static const std::string empty;
  return xml ? xml->getNextPageUrl() : json ? json->getNextPageUrl() : empty;
}

const std::string& OpdsFeedParser::getPrevPageUrl() const {
  static const std::string empty;
  return xml ? xml->getPrevPageUrl() : json ? json->getPrevPageUrl() : empty;
}
