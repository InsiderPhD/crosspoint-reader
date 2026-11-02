// Adapted from CrossInk's OpdsJsonParser / FreeInk Opds2Parser; see
// LICENSE.FreeInk for the MIT notice. DRM (LCP) and purchase paths removed.
#include "OpdsJsonParser.h"

#include <Logging.h>

#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t MAX_TITLE_CHARS = 160, MAX_AUTHOR_CHARS = 120, MAX_ID_CHARS = 128;
constexpr size_t MAX_HREF_CHARS = 500;

// The tokenizer hands longer strings only to onStringPrefix, so every stored
// field must fit its buffer or long values would vanish rather than be cut.
static_assert(MAX_HREF_CHARS < StreamingJsonParser::TOKEN_BUF_SIZE - 1, "href limit must fit the JSON token buffer");

// Acquisition preference: open access > plain acquisition > borrow/subscribe.
// Buy links, samples and previews are not acquisitions this firmware can use.
int acquisitionRank(const char* rel) {
  if (strstr(rel, "opds-spec.org/acquisition") != nullptr) {
    if (strstr(rel, "/sample") || strstr(rel, "/preview") || strstr(rel, "/buy")) return -1;
    if (strstr(rel, "/open-access")) return 3;
    if (strstr(rel, "/borrow") || strstr(rel, "/subscribe")) return 1;
    return 2;
  }
  if (!strcmp(rel, "download") || !strcmp(rel, "open-access")) return 3;
  if (!strcmp(rel, "acquisition")) return 2;
  if (!strcmp(rel, "borrow") || !strcmp(rel, "subscribe")) return 1;
  return -1;
}

void assignBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  target.assign(value, len < maxLen ? len : maxLen);
}
}  // namespace

OpdsJsonParser::OpdsJsonParser(const bool publication)
    : parser(JsonCallbacks{this, &sOnKey, &sOnString, &sOnNumber, &sOnBool, nullptr, &sOnObjectStart, &sOnObjectEnd,
                           &sOnArrayStart, &sOnArrayEnd, &sOnStringPrefix}),
      publicationDocument(publication) {}

void OpdsJsonParser::setPreferredLanguage(const char* lang) {
  size_t i = 0;
  for (; lang && lang[i] && i < sizeof(preferredLang) - 1; ++i) {
    preferredLang[i] = static_cast<char>((lang[i] >= 'A' && lang[i] <= 'Z') ? lang[i] + 32 : lang[i]);
  }
  preferredLang[i] = '\0';
}

void OpdsJsonParser::captureDescriptionFor(const std::string& href, const size_t maxBytes) {
  descriptionHref = href;
  captureDescription = true;
  capturedDescription.clear();
  description.begin(maxBytes);
}

// "en" matches "en" and "en-US" (not "eng"), case-insensitively.
bool OpdsJsonParser::pendingKeyIsPreferred() const {
  if (!preferredLang[0]) return false;
  size_t i = 0;
  for (; preferredLang[i]; ++i) {
    const char k = pendingKey[i];
    if (static_cast<char>((k >= 'A' && k <= 'Z') ? k + 32 : k) != preferredLang[i]) return false;
  }
  return pendingKey[i] == '\0' || pendingKey[i] == '-';
}

size_t OpdsJsonParser::write(const uint8_t c) { return write(&c, 1); }

// A short write makes the HTTP sink stop the transfer once parsing failed.
size_t OpdsJsonParser::write(const uint8_t* data, const size_t length) {
  if (errorOccured || (!data && length)) {
    errorOccured = true;
    return 0;
  }
  parser.feed(reinterpret_cast<const char*>(data), length);
  if (parser.hasError()) {
    errorOccured = true;
    LOG_DBG("OPDS2", "JSON parse error");
  }
  return error() ? 0 : length;
}

// A body that ends before its root object closes is a truncated feed.
void OpdsJsonParser::flush() {
  if (!sawRoot || depth != 0) {
    LOG_DBG("OPDS2", "Incomplete JSON document");
    errorOccured = true;
  }
}

bool OpdsJsonParser::error() const { return errorOccured || parser.hasError(); }

bool OpdsJsonParser::append(OpdsEntry&& entry) {
  // A description-only pass keeps no rows (see OpdsParser::keepEntries).
  if (captureDescription && !publicationDocument) return true;
  const size_t bytes = entry.title.size() + entry.author.size() + entry.href.size() + entry.id.size();
  if (entries.size() >= MAX_ENTRIES || bytes > MAX_ENTRY_STRING_BYTES - entryStringBytes) {
    feedTruncated = true;
    return false;
  }
  // Grows in steps of 16 rows at most: a catalog page is rarely longer, and
  // an exact-fit reserve avoids the vector's doubling copy.
  if (entries.size() == entries.capacity()) entries.reserve(entries.size() + 16);
  entryStringBytes += bytes;
  entries.push_back(std::move(entry));
  return true;
}

void OpdsJsonParser::resetLink() {
  link.href.clear();
  link.title.clear();
  link.relSearch = link.relNext = link.relPrev = link.relSelf = false;
  link.acqRank = -1;
  link.typeEpub = link.typePubDoc = link.templated = false;
  link.numberOfItems = -1;
}

void OpdsJsonParser::applyRel(const char* rel) {
  if (!strcmp(rel, "search")) link.relSearch = true;
  if (!strcmp(rel, "next")) link.relNext = true;
  if (!strcmp(rel, "previous") || !strcmp(rel, "prev")) link.relPrev = true;
  if (!strcmp(rel, "self")) link.relSelf = true;
  const int rank = acquisitionRank(rel);
  if (rank > link.acqRank) link.acqRank = rank;
}

OpdsJsonParser::Scope OpdsJsonParser::scopeForChild(const Scope parent, const bool isObject) const {
  switch (parent) {
    case Scope::FEED:
      if (isObject && !strcmp(pendingKey, "metadata")) return Scope::FEED_META;
      if (!isObject) {
        if (!strcmp(pendingKey, "links")) return Scope::FEED_LINKS;
        if (!strcmp(pendingKey, "navigation")) return Scope::NAV;
        if (!strcmp(pendingKey, "publications")) return Scope::PUBS;
        if (!strcmp(pendingKey, "groups")) return Scope::GROUPS;
        if (!strcmp(pendingKey, "facets")) return Scope::FACETS;
      }
      return Scope::SKIP;
    case Scope::GROUP:
      if (isObject && !strcmp(pendingKey, "metadata")) return Scope::GROUP_META;
      if (!isObject) {
        if (!strcmp(pendingKey, "links")) return Scope::GROUP_LINKS;
        if (!strcmp(pendingKey, "navigation")) return Scope::NAV;
        if (!strcmp(pendingKey, "publications")) return Scope::PUBS;
      }
      return Scope::SKIP;
    case Scope::FEED_LINKS:
      return isObject ? Scope::FEED_LINK : Scope::SKIP;
    case Scope::GROUP_LINKS:
      return isObject ? Scope::GROUP_LINK : Scope::SKIP;
    case Scope::FEED_LINK:
    case Scope::GROUP_LINK:
    case Scope::PUB_LINK:
      return (!isObject && !strcmp(pendingKey, "rel")) ? Scope::LINK_REL : Scope::SKIP;
    case Scope::FACET_LINK:
      if (!isObject && !strcmp(pendingKey, "rel")) return Scope::LINK_REL;
      if (isObject && !strcmp(pendingKey, "properties")) return Scope::FACET_PROPS;
      return Scope::SKIP;
    case Scope::NAV:
      return isObject ? Scope::NAV_LINK : Scope::SKIP;
    case Scope::PUBS:
      return isObject ? Scope::PUB : Scope::SKIP;
    case Scope::PUB:
      if (isObject && !strcmp(pendingKey, "metadata")) return Scope::PUB_META;
      if (!isObject && !strcmp(pendingKey, "links")) return Scope::PUB_LINKS;
      return Scope::SKIP;
    case Scope::PUB_META:
      if (!strcmp(pendingKey, "author")) return isObject ? Scope::AUTHOR : Scope::AUTHOR_ARR;
      if (isObject && !strcmp(pendingKey, "title")) return Scope::PUB_TITLE;
      return Scope::SKIP;
    case Scope::AUTHOR_ARR:
      return isObject ? Scope::AUTHOR : Scope::SKIP;
    case Scope::PUB_LINKS:
      return isObject ? Scope::PUB_LINK : Scope::SKIP;
    case Scope::GROUPS:
      return isObject ? Scope::GROUP : Scope::SKIP;
    case Scope::FACETS:
      return isObject ? Scope::FACET : Scope::SKIP;
    case Scope::FACET:
      return (!isObject && !strcmp(pendingKey, "links")) ? Scope::FACET_LINKS : Scope::SKIP;
    case Scope::FACET_LINKS:
      return isObject ? Scope::FACET_LINK : Scope::SKIP;
    default:
      return Scope::SKIP;
  }
}

void OpdsJsonParser::onContainerStart(const bool isObject) {
  if (depth >= MAX_DEPTH) {
    errorOccured = true;
    return;
  }
  Scope next;
  if (!sawRoot) {
    sawRoot = true;
    if (!isObject) errorOccured = true;
    next = isObject ? (publicationDocument ? Scope::PUB : Scope::FEED) : Scope::SKIP;
  } else {
    if (depth == 0) {  // a second root value
      errorOccured = true;
      return;
    }
    next = scopeForChild(current(), isObject);
  }
  switch (next) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      resetLink();
      break;
    case Scope::PUB:
      currentEntry = OpdsEntry{};
      pubAcqRank = -1;
      pubHasDirect = false;
      pubSelfHref.clear();
      if (captureDescription) description.reset();
      break;
    case Scope::GROUP:
      groupTitle.clear();
      groupSelfHref.clear();
      groupPubCount = 0;
      break;
    case Scope::PUB_TITLE:
      localizedTitleLocked = false;
      break;
    default:
      break;
  }
  stack[depth++] = next;
  pendingKey[0] = '\0';
}

void OpdsJsonParser::onContainerEnd() {
  if (depth == 0) return;
  const Scope closed = stack[--depth];
  pendingKey[0] = '\0';
  switch (closed) {
    case Scope::FEED_LINK:
      commitFeedLink();
      break;
    case Scope::NAV_LINK:
      commitNavLink();
      break;
    case Scope::PUB_LINK:
      commitPubLink();
      break;
    case Scope::GROUP_LINK:
      commitGroupLink();
      break;
    case Scope::FACET_LINK:
      commitFacetLink();
      break;
    case Scope::PUB:
      commitPublication();
      break;
    case Scope::GROUP:
      endGroup();
      break;
    default:
      break;
  }
}

void OpdsJsonParser::onDescription(const char* value, const size_t len) {
  if (captureDescription && description.empty()) description.append(value, len);
}

void OpdsJsonParser::onStringValue(const char* value, const size_t len) {
  switch (current()) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      if (!strcmp(pendingKey, "href")) {
        assignBounded(link.href, value, len, MAX_HREF_CHARS);
      } else if (!strcmp(pendingKey, "title")) {
        assignBounded(link.title, value, len, MAX_TITLE_CHARS);
      } else if (!strcmp(pendingKey, "rel")) {
        applyRel(value);
      } else if (!strcmp(pendingKey, "type")) {
        if (!strcmp(value, "application/epub+zip")) link.typeEpub = true;
        if (!strcmp(value, "application/opds-publication+json")) link.typePubDoc = true;
      }
      break;
    case Scope::LINK_REL:
      applyRel(value);
      break;
    case Scope::GROUP_META:
      if (!strcmp(pendingKey, "title")) assignBounded(groupTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::PUB_META:
      if (!strcmp(pendingKey, "title")) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      } else if (!strcmp(pendingKey, "author")) {
        if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      } else if (!strcmp(pendingKey, "identifier")) {
        assignBounded(currentEntry.id, value, len, MAX_ID_CHARS);
      } else if (!strcmp(pendingKey, "description")) {
        onDescription(value, len);
      }
      break;
    case Scope::PUB_TITLE:
      // Localised title object: the UI language's translation, else the first.
      if (pendingKeyIsPreferred()) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
        localizedTitleLocked = true;
      } else if (!localizedTitleLocked && currentEntry.title.empty()) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      }
      break;
    case Scope::AUTHOR:
      if (!strcmp(pendingKey, "name") && currentEntry.author.empty()) {
        assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      }
      break;
    case Scope::AUTHOR_ARR:
      if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      break;
    default:
      break;
  }
}

void OpdsJsonParser::commitFeedLink() {
  if (link.href.empty()) return;
  if (link.relSearch && (link.templated || link.href.find('{') != std::string::npos)) {
    if (searchTemplate.empty()) searchTemplate = link.href;
    return;
  }
  if (link.relNext && nextPageUrl.empty()) nextPageUrl = link.href;
  if (link.relPrev && prevPageUrl.empty()) prevPageUrl = link.href;
}

void OpdsJsonParser::commitNavLink() {
  if (link.href.empty() || link.title.empty()) return;
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title = std::move(link.title);
  entry.href = std::move(link.href);
  append(std::move(entry));
}

void OpdsJsonParser::commitGroupLink() {
  if (link.relSelf && groupSelfHref.empty()) groupSelfHref = std::move(link.href);
}

void OpdsJsonParser::commitFacetLink() {
  if (link.href.empty() || link.title.empty()) return;
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title = std::move(link.title);
  entry.href = std::move(link.href);
  entry.count = link.numberOfItems;
  append(std::move(entry));
}

// Keeps the best usable link of the current publication: a direct EPUB beats
// an indirect one, then the higher acquisition rank wins. A rel="self"
// publication document is remembered as the fallback for a publication whose
// only route to a book goes through its detail document.
void OpdsJsonParser::commitPubLink() {
  if (link.href.empty()) return;
  if (link.relSelf && link.typePubDoc) {
    if (pubSelfHref.empty()) pubSelfHref = std::move(link.href);
    return;
  }
  if (link.acqRank < 0 || !(link.typeEpub || link.typePubDoc)) return;
  const bool direct = link.typeEpub;
  const bool better =
      currentEntry.href.empty() || (direct && !pubHasDirect) || (direct == pubHasDirect && link.acqRank > pubAcqRank);
  if (!better) return;
  currentEntry.href = std::move(link.href);
  currentEntry.indirect = !direct;
  pubAcqRank = link.acqRank;
  pubHasDirect = direct;
}

void OpdsJsonParser::commitPublication() {
  if (currentEntry.href.empty() && !pubSelfHref.empty()) {
    currentEntry.href = std::move(pubSelfHref);
    currentEntry.indirect = true;
  }
  if (captureDescription && capturedDescription.empty() &&
      (publicationDocument || (!currentEntry.href.empty() && currentEntry.href == descriptionHref))) {
    capturedDescription = description.finish();
  }
  // Listable with a title and some way to the book (publication documents
  // keep their one entry regardless, for its description).
  if (currentEntry.title.empty() && !publicationDocument) return;
  if (currentEntry.href.empty() && !publicationDocument) return;
  // A group in a feed is a preview: its "see all" row reaches the rest.
  if (depth >= 2 && stack[depth - 2] == Scope::GROUP && ++groupPubCount > MAX_GROUP_PREVIEW) return;
  currentEntry.type = OpdsEntryType::BOOK;
  append(std::move(currentEntry));
  currentEntry = OpdsEntry{};
}

void OpdsJsonParser::endGroup() {
  // Field order in a group is not guaranteed, so its "see all" row is added
  // once the whole group has been read.
  if (groupSelfHref.empty()) return;
  OpdsEntry seeAll;
  seeAll.type = OpdsEntryType::NAVIGATION;
  seeAll.title = groupTitle.empty() ? groupSelfHref : groupTitle;
  seeAll.href = std::move(groupSelfHref);
  append(std::move(seeAll));
  groupSelfHref.clear();
}

void OpdsJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  const size_t max = sizeof(self->pendingKey) - 1;
  if (len > max) len = max;
  memcpy(self->pendingKey, key, len);
  self->pendingKey[len] = '\0';
}

void OpdsJsonParser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<OpdsJsonParser*>(ctx)->onStringValue(value, len);
}

// Only a description is worth a prefix; any other oversized field is dropped.
void OpdsJsonParser::sOnStringPrefix(void* ctx, const char* value, const size_t len) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  if (self->current() == Scope::PUB_META && !strcmp(self->pendingKey, "description")) self->onDescription(value, len);
}

void OpdsJsonParser::sOnNumber(void* ctx, const char* value, size_t) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  if (self->current() == Scope::FACET_PROPS && !strcmp(self->pendingKey, "numberOfItems")) {
    self->link.numberOfItems = static_cast<int32_t>(strtol(value, nullptr, 10));
  }
}

void OpdsJsonParser::sOnBool(void* ctx, const bool value) {
  auto* self = static_cast<OpdsJsonParser*>(ctx);
  if (self->current() == Scope::FEED_LINK && !strcmp(self->pendingKey, "templated")) self->link.templated = value;
}

void OpdsJsonParser::sOnObjectStart(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerStart(true); }
void OpdsJsonParser::sOnObjectEnd(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerEnd(); }
void OpdsJsonParser::sOnArrayStart(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerStart(false); }
void OpdsJsonParser::sOnArrayEnd(void* ctx) { static_cast<OpdsJsonParser*>(ctx)->onContainerEnd(); }
