#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <strings.h>

#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

#include "../BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char itemCacheFile[] = "/.items.bin";

// Calibre embeds custom-column data as a JSON blob in a <meta> content attribute.
// Pull out just the "#value#" field without a full JSON parser: handles a quoted
// string ("#value#": "Shelf") or a list ("#value#": ["A","B"] -> "A, B"). Returns
// empty for null/absent. Bounded — only the extracted value is copied out, not the blob.
std::string extractCalibreCustomValue(const char* json) {
  if (!json) return {};
  const std::string_view sv{json};
  const auto key = sv.find("\"#value#\"");
  if (key == std::string_view::npos) return {};

  size_t i = key + 9;  // past the key token
  while (i < sv.size() && (sv[i] == ' ' || sv[i] == ':' || sv[i] == '\t')) i++;
  if (i >= sv.size()) return {};

  const auto readQuoted = [&](size_t pos, std::string& out) -> size_t {
    // pos points at the opening quote; appends unescaped contents to out, returns
    // index just past the closing quote (or npos on malformed input).
    pos++;
    while (pos < sv.size()) {
      const char c = sv[pos];
      if (c == '\\' && pos + 1 < sv.size()) {
        out.push_back(sv[pos + 1]);
        pos += 2;
        continue;
      }
      if (c == '"') return pos + 1;
      out.push_back(c);
      pos++;
    }
    return std::string_view::npos;
  };

  std::string value;
  if (sv[i] == '"') {
    readQuoted(i, value);
  } else if (sv[i] == '[') {
    i++;
    while (i < sv.size() && sv[i] != ']') {
      if (sv[i] == '"') {
        std::string item;
        i = readQuoted(i, item);
        if (i == std::string_view::npos) break;
        if (!value.empty()) value.append(", ");
        value.append(item);
      } else {
        i++;
      }
    }
  }
  return value;
}

bool startsWithIgnoreCase(const std::string& s, const char* prefix) {
  const size_t n = strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
  }
  return true;
}

// ISBN-13 for a <dc:identifier>, or "" when it isn't one. Accepts "urn:isbn:" /
// "isbn:" prefixes and hyphen/space separators, and converts ISBN-10 to 13 so
// lookups need only one form. The checksum is required even when the scheme
// says ISBN: it is what separates a real ISBN from an ASIN, UUID or Calibre id
// that happens to be all digits.
std::string isbn13FromIdentifier(const std::string& raw) {
  size_t start = 0;
  while (start < raw.size() && isspace(static_cast<unsigned char>(raw[start]))) start++;
  const std::string text = raw.substr(start);
  size_t pos = 0;
  if (startsWithIgnoreCase(text, "urn:isbn:")) {
    pos = 9;
  } else if (startsWithIgnoreCase(text, "isbn:")) {
    pos = 5;
  }

  char digits[14];
  size_t count = 0;
  for (; pos < text.size(); pos++) {
    const char c = text[pos];
    if (c == '-' || c == ' ') continue;
    if (isspace(static_cast<unsigned char>(c))) break;  // trailing whitespace
    const bool isCheckX = (c == 'X' || c == 'x') && count == 9;
    if (!isdigit(static_cast<unsigned char>(c)) && !isCheckX) return {};
    if (count >= 13) return {};
    digits[count++] = isCheckX ? 'X' : c;
  }

  if (count == 13) {
    if (memcmp(digits, "978", 3) != 0 && memcmp(digits, "979", 3) != 0) return {};
    int sum = 0;
    for (size_t i = 0; i < 13; i++) {
      sum += (digits[i] - '0') * ((i % 2) ? 3 : 1);
    }
    return (sum % 10 == 0) ? std::string(digits, 13) : std::string();
  }

  if (count == 10) {
    int sum = 0;
    for (size_t i = 0; i < 10; i++) {
      const int value = digits[i] == 'X' ? 10 : digits[i] - '0';
      sum += value * static_cast<int>(10 - i);
    }
    if (sum % 11 != 0) return {};
    char isbn13[13];
    memcpy(isbn13, "978", 3);
    memcpy(isbn13 + 3, digits, 9);
    int sum13 = 0;
    for (size_t i = 0; i < 12; i++) {
      sum13 += (isbn13[i] - '0') * ((i % 2) ? 3 : 1);
    }
    isbn13[12] = static_cast<char>('0' + (10 - sum13 % 10) % 10);
    return std::string(isbn13, 13);
  }
  return {};
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->state == START && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:description") == 0) {
    self->descInTag = false;
    self->state = IN_BOOK_DESCRIPTION;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:subject") == 0) {
    // Multiple dc:subject elements -> join into a single comma-separated tag string.
    if (!self->tags.empty()) {
      self->tags.append(", ");
    }
    self->state = IN_BOOK_SUBJECT;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:publisher") == 0) {
    self->state = IN_BOOK_PUBLISHER;
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:date") == 0) {
    // Capture only the first dc:date (the publication date); Calibre may emit others.
    if (self->pubDate.empty()) {
      self->state = IN_BOOK_DATE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(name, "dc:identifier") == 0) {
    // Books carry several identifiers (UUID, ASIN, Calibre id); keep scanning
    // until one is an ISBN, then ignore the rest.
    if (self->isbn.empty()) {
      self->identifierText.clear();
      self->identifierSchemeIsbn = false;
      for (int i = 0; atts[i]; i += 2) {
        if ((strcmp(atts[i], "opf:scheme") == 0 || strcmp(atts[i], "scheme") == 0) &&
            strcasecmp(atts[i + 1], "ISBN") == 0) {
          self->identifierSchemeIsbn = true;
        }
      }
      self->state = IN_BOOK_IDENTIFIER;
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Sort item index for binary search if we have enough items
    if (self->itemIndex.size() >= LARGE_SPINE_THRESHOLD) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "meta") == 0 || strcmp(name, "opf:meta") == 0)) {
    const char* metaName = nullptr;
    const char* metaContent = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0) {
        metaName = atts[i + 1];
      } else if (strcmp(atts[i], "content") == 0) {
        metaContent = atts[i + 1];
      }
    }

    if (metaName && metaContent) {
      if (strcmp(metaName, "cover") == 0) {
        self->coverItemId = metaContent;
      } else if (strcmp(metaName, "calibre:series") == 0) {
        // Calibre's custom series metadata. EPUB3-only belongs-to-collection is not
        // handled here; Calibre always writes the calibre:* pair for back-compat.
        self->seriesName = metaContent;
      } else if (strcmp(metaName, "calibre:series_index") == 0) {
        self->seriesIndex = metaContent;
      } else if (strcmp(metaName, "calibre:rating") == 0) {
        self->rating = metaContent;
      } else if (strcmp(metaName, "calibre:user_metadata:#bookshelf") == 0) {
        // BookFusion's "bookshelf" Calibre custom column. The content is a JSON blob
        // describing the column; we pull only the "#value#" out (string or list).
        self->bookshelf = extractCalibreCustomValue(metaContent);
      }
    }
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "item") == 0 || strcmp(name, "opf:item") == 0)) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex.push_back(entry);
    }

    // Write items down to SD card
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      self->coverItemHref = href;
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && (strcmp(name, "itemref") == 0 || strcmp(name, "opf:itemref") == 0)) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            // Fast path: binary search
            uint32_t targetHash = fnvHash(idref);
            uint16_t targetLen = static_cast<uint16_t>(idref.size());

            auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                       ItemIndexEntry{targetHash, targetLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            // Check for match (may need to check a few due to hash collisions)
            while (it != self->itemIndex.end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              std::string itemId;
              serialization::readString(self->tempItemStore, itemId);
              if (itemId == idref) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            // Slow path: linear scan (for small manifests, keeps original behavior)
            // TODO: This lookup is slow as need to scan through all items each time.
            //       It can take up to 200ms per item when getting to 1500 items.
            self->tempItemStore.seek(0);
            std::string itemId;
            while (self->tempItemStore.available()) {
              serialization::readString(self->tempItemStore, itemId);
              serialization::readString(self->tempItemStore, href);
              if (itemId == idref) {
                found = true;
                break;
              }
            }
          }

          if (found && self->cache) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && (strcmp(name, "reference") == 0 || strcmp(name, "opf:reference") == 0)) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      if (type == "text" || (type == "start" && !self->textReferenceHref.empty())) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      // Join multiple <dc:creator> elements with " & " (not ", "): a name is written
      // "Last, First", so a comma is ambiguous with the intra-name separator. Using '&'
      // lets the Authors folder view split co-authors correctly (GroupBrowserActivity
      // splits author keys on '&'). This is also the Calibre display convention.
      self->author.append(" & ");
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_SUBJECT) {
    self->tags.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_PUBLISHER) {
    self->publisher.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_DATE) {
    self->pubDate.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER) {
    // An ISBN with prefix and separators is under 32 bytes; anything longer is
    // some other identifier and would fail validation anyway.
    constexpr size_t MAX_IDENTIFIER_BYTES = 64;
    if (self->identifierText.size() + static_cast<size_t>(len) <= MAX_IDENTIFIER_BYTES) {
      self->identifierText.append(s, len);
    }
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION) {
    // Calibre stores comments as escaped HTML; expat hands us the unescaped text
    // (literal "<p>...</p>"). Strip tags and collapse whitespace as we stream, and
    // stop once we hit the cap so a pathological description can't grow unbounded.
    for (int i = 0; i < len; i++) {
      if (self->description.size() >= Epub::MAX_DESCRIPTION_BYTES) break;
      const char c = s[i];
      if (self->descInTag) {
        if (c == '>') self->descInTag = false;
        continue;
      }
      if (c == '<') {
        self->descInTag = true;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        if (!self->description.empty() && self->description.back() != ' ') {
          self->description.push_back(' ');
        }
        continue;
      }
      self->description.push_back(c);
    }
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->state == IN_SPINE && (strcmp(name, "spine") == 0 || strcmp(name, "opf:spine") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && (strcmp(name, "guide") == 0 || strcmp(name, "opf:guide") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && (strcmp(name, "manifest") == 0 || strcmp(name, "opf:manifest") == 0)) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(name, "dc:title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(name, "dc:language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_DESCRIPTION && strcmp(name, "dc:description") == 0) {
    self->descInTag = false;
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_SUBJECT && strcmp(name, "dc:subject") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_PUBLISHER && strcmp(name, "dc:publisher") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_DATE && strcmp(name, "dc:date") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER && strcmp(name, "dc:identifier") == 0) {
    std::string isbn13 = isbn13FromIdentifier(self->identifierText);
    if (!isbn13.empty()) {
      LOG_DBG("COF", "ISBN %s (scheme attr: %d)", isbn13.c_str(), self->identifierSchemeIsbn ? 1 : 0);
      self->isbn = std::move(isbn13);
    }
    self->identifierText.clear();
    self->identifierText.shrink_to_fit();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && (strcmp(name, "metadata") == 0 || strcmp(name, "opf:metadata") == 0)) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && (strcmp(name, "package") == 0 || strcmp(name, "opf:package") == 0)) {
    self->state = START;
    return;
  }
}
