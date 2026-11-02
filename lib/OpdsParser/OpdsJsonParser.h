#pragma once
#include <Print.h>
#include <StreamingJsonParser.h>

#include <cstdint>
#include <string>
#include <vector>

#include "OpdsDescription.h"
#include "OpdsParser.h"

/**
 * OPDS 2 (JSON) feed parser: navigation, publications, groups and facets.
 * Adapted from CrossInk's OpdsJsonParser (itself from FreeInk's Opds2Parser,
 * MIT, see LICENSE.FreeInk) onto this firmware's OpdsEntry list, with every
 * DRM path removed: only plain EPUB acquisitions
 * (application/epub+zip) and indirect ones that lead to a publication
 * document (application/opds-publication+json) are kept. LCP licence links,
 * buy-only links and samples are ignored, so a publication offering nothing
 * else is not listed.
 *
 * Streaming SAX over StreamingJsonParser: the document is never held, and the
 * output is bounded (MAX_ENTRIES rows, MAX_ENTRY_STRING_BYTES of their text)
 * so a huge catalog cannot exhaust the C3 heap mid-TLS.
 *
 * Rows: navigation links and facets become NAVIGATION rows (facet counts in
 * OpdsEntry::count); a group contributes up to MAX_GROUP_PREVIEW publications
 * plus a "see all" row for its self link; publications become BOOK rows whose
 * href is the EPUB, or, with OpdsEntry::indirect, the publication document to
 * fetch for it.
 *
 * In publication mode the root object is one publication (an OPDS 2
 * publication document): getEntries() then holds at most that one book.
 */
class OpdsJsonParser final : public Print {
 public:
  static constexpr size_t MAX_ENTRIES = 100;
  static constexpr size_t MAX_ENTRY_STRING_BYTES = 24 * 1024;

  explicit OpdsJsonParser(bool publicationDocument = false);

  OpdsJsonParser(const OpdsJsonParser&) = delete;
  OpdsJsonParser& operator=(const OpdsJsonParser&) = delete;

  // Preferred UI language (primary subtag, e.g. "en") for localised titles.
  void setPreferredLanguage(const char* lang);
  // As OpdsParser::captureDescriptionFor; in publication mode `href` is
  // ignored and the document's own description is captured.
  void captureDescriptionFor(const std::string& href, size_t maxBytes);
  std::string takeDescription() { return std::move(capturedDescription); }

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  std::vector<OpdsEntry> getEntries() && { return std::move(entries); }
  const std::vector<OpdsEntry>& getEntries() const& { return entries; }
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }

 private:
  // Role of each open JSON container, from its parent and the key that
  // opened it. SKIP swallows whole subtrees (images, unknown extensions).
  enum class Scope : uint8_t {
    FEED,
    FEED_META,
    FEED_LINKS,
    FEED_LINK,
    LINK_REL,
    NAV,
    NAV_LINK,
    PUBS,
    PUB,
    PUB_META,
    PUB_TITLE,
    AUTHOR,
    AUTHOR_ARR,
    PUB_LINKS,
    PUB_LINK,
    GROUPS,
    GROUP,
    GROUP_META,
    GROUP_LINKS,
    GROUP_LINK,
    FACETS,
    FACET,
    FACET_LINKS,
    FACET_LINK,
    FACET_PROPS,
    SKIP,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnStringPrefix(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onContainerStart(bool isObject);
  void onContainerEnd();
  void onStringValue(const char* value, size_t len);
  void onDescription(const char* value, size_t len);
  Scope scopeForChild(Scope parent, bool isObject) const;
  Scope current() const { return depth > 0 ? stack[depth - 1] : Scope::FEED; }
  bool pendingKeyIsPreferred() const;

  void resetLink();
  void applyRel(const char* rel);
  void commitFeedLink();
  void commitNavLink();
  void commitGroupLink();
  void commitFacetLink();
  void commitPubLink();
  void commitPublication();
  void endGroup();
  bool append(OpdsEntry&& entry);

  StreamingJsonParser parser;
  static constexpr uint8_t MAX_DEPTH = StreamingJsonParser::MAX_NESTING + 1;
  Scope stack[MAX_DEPTH];
  uint8_t depth = 0;
  bool sawRoot = false;
  bool errorOccured = false;
  bool feedTruncated = false;
  bool publicationDocument = false;

  char pendingKey[24] = {0};
  char preferredLang[12] = {0};
  bool localizedTitleLocked = false;

  std::vector<OpdsEntry> entries;
  size_t entryStringBytes = 0;
  OpdsEntry currentEntry;

  // The link object being read; the scope on commit says which kind it is.
  struct {
    std::string href;
    std::string title;
    bool relSearch = false;
    bool relNext = false;
    bool relPrev = false;
    bool relSelf = false;
    int acqRank = -1;
    bool typeEpub = false;
    bool typePubDoc = false;  // application/opds-publication+json
    bool templated = false;
    int32_t numberOfItems = -1;
  } link;
  int pubAcqRank = -1;
  bool pubHasDirect = false;
  std::string pubSelfHref;  // publication document, used when no acquisition is direct

  static constexpr int MAX_GROUP_PREVIEW = 5;
  std::string groupTitle;
  std::string groupSelfHref;
  int groupPubCount = 0;

  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;

  // Description capture (captureDescriptionFor).
  std::string descriptionHref;
  bool captureDescription = false;
  OpdsDescription description;
  std::string capturedDescription;
};
