#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstring>

namespace {
// Only short "<N> books" summaries carry a count. The summary text is collected
// into a buffer one char longer than this, so a descriptive sentence overflows
// it and is rejected without ever being stored.
constexpr size_t MAX_SUMMARY_COUNT_CHARS = 32;

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isDigit(const char c) { return c >= '0' && c <= '9'; }

// Consumes a non-negative decimal integer at `p`; false on no digits or overflow.
bool consumeCount(const char*& p, int32_t& out) {
  if (!isDigit(*p)) return false;
  int32_t value = 0;
  for (; isDigit(*p); ++p) {
    if (value > (INT32_MAX - (*p - '0')) / 10) return false;
    value = value * 10 + (*p - '0');
  }
  out = value;
  return true;
}

// thr:count attribute value: digits only; -1 otherwise.
int32_t parseAttributeCount(const char* text) {
  int32_t value = -1;
  if (!text || !consumeCount(text, value) || *text != '\0') return -1;
  return value;
}

// "<N> book(s)" summaries; a descriptive sentence that happens to start with a
// number yields -1.
int32_t parseSummaryCount(const std::string& summary) {
  if (summary.size() > MAX_SUMMARY_COUNT_CHARS) return -1;
  const char* p = summary.c_str();
  while (isSpace(*p)) ++p;
  int32_t value = -1;
  if (!consumeCount(p, value) || (*p != ' ' && *p != '\t')) return -1;
  while (isSpace(*p)) ++p;
  if (strncmp(p, "books", 5) == 0) {
    p += 5;
  } else if (strncmp(p, "book", 4) == 0) {
    p += 4;
  } else {
    return -1;
  }
  while (isSpace(*p)) ++p;
  return *p == '\0' ? value : -1;
}
}  // namespace

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
  }
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    void* const buf = XML_GetBuffer(parser, chunkSize);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inSummary = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        if (rel && type && strstr(rel, "opds-spec.org/acquisition") != nullptr &&
            strcmp(type, "application/epub+zip") == 0) {
          self->currentEntry.type = OpdsEntryType::BOOK;
          self->currentEntry.href = href;
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
            // Atom threading extension: thr:count="N", under whatever prefix
            // the feed bound the namespace to.
            for (int i = 0; atts[i]; i += 2) {
              const char* colon = strrchr(atts[i], ':');
              if (colon && strcmp(colon + 1, "count") == 0) {
                const int32_t count = parseAttributeCount(atts[i + 1]);
                if (count >= 0) self->currentEntry.count = count;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
    self->inSummary = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      // A book's summary is its blurb, never a category size.
      if (self->currentEntry.type != OpdsEntryType::NAVIGATION) self->currentEntry.count = -1;
      self->entries.push_back(self->currentEntry);
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = self->currentText;
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = self->currentText;
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = self->currentText;
      self->inId = false;
    } else if (strcmp(name, "summary") == 0 || strstr(name, ":summary") != nullptr) {
      // thr:count on the link wins over a summary count.
      if (self->inSummary && self->currentEntry.count < 0) {
        self->currentEntry.count = parseSummaryCount(self->currentText);
      }
      self->inSummary = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId) {
    self->currentText.append(s, len);
  } else if (self->inSummary) {
    // Drop leading whitespace and collapse runs, so an indented
    // "\n    12713 books\n  " still fits the count-sized buffer while a real
    // sentence overflows it (and stops being collected).
    std::string& text = self->currentText;
    for (int i = 0; i < len && text.size() <= MAX_SUMMARY_COUNT_CHARS; ++i) {
      if (isSpace(s[i]) && (text.empty() || isSpace(text.back()))) continue;
      text.push_back(s[i]);
    }
  }
}
