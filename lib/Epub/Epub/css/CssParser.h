#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - ID selectors: #id, element#id
 *   - Descendant / child chains of those (div.note p, section > p.first), up to
 *     MAX_CONTEXT_COMPOUNDS parts, held in a small bounded list (see contextRules_)
 *   - Grouped: selector1, selector2 { }
 * Rules apply in specificity order (ids, classes, elements); equal specificity
 * puts a descendant/child rule after a simple one.
 *
 * Not supported (silently ignored):
 *   - Sibling combinators (+, ~), attribute selectors, *
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */

// One element as selector matching sees it. Views into storage owned elsewhere.
struct CssElementView {
  std::string_view tag;
  std::string_view classes;  // the raw class attribute, whitespace separated
  std::string_view id;
};

// The open ancestors of the element being styled, outermost first, for descendant / child
// selectors. ChapterHtmlSlimParser keeps one only while the book has such rules. All the
// strings live in one buffer, so an open element costs an Entry (12 bytes) and its names,
// with no per-element allocation; both are freed with the parser at the end of a section.
class CssAncestorStack {
 public:
  // Names past MAX_CHARS are dropped (the element still counts for depth bookkeeping).
  static constexpr size_t MAX_CHARS = 4096;

  void push(const int depth, const std::string_view tag, const std::string_view classes, const std::string_view id) {
    if (entries_.empty()) {
      entries_.reserve(16);
      chars_.reserve(256);
    }
    Entry e{};
    e.depth = static_cast<int16_t>(depth);
    if (chars_.size() + tag.size() + classes.size() + id.size() <= MAX_CHARS) {
      e.tagOff = static_cast<uint16_t>(chars_.size());
      e.tagLen = static_cast<uint16_t>(tag.size());
      chars_.append(tag);
      e.classOff = static_cast<uint16_t>(chars_.size());
      e.classLen = static_cast<uint16_t>(classes.size());
      chars_.append(classes);
      e.idOff = static_cast<uint16_t>(chars_.size());
      e.idLen = static_cast<uint16_t>(id.size());
      chars_.append(id);
    } else {
      e.tagOff = e.classOff = e.idOff = static_cast<uint16_t>(chars_.size());
    }
    entries_.push_back(e);
  }
  // Pops every element opened at `depth` or deeper.
  void popTo(const int depth) {
    while (!entries_.empty() && entries_.back().depth >= depth) {
      chars_.resize(entries_.back().tagOff);
      entries_.pop_back();
    }
  }
  [[nodiscard]] size_t size() const { return entries_.size(); }
  [[nodiscard]] CssElementView at(const size_t i) const {
    const Entry& e = entries_[i];
    const std::string_view all(chars_);
    return {all.substr(e.tagOff, e.tagLen), all.substr(e.classOff, e.classLen), all.substr(e.idOff, e.idLen)};
  }

 private:
  struct Entry {
    int16_t depth;
    uint16_t tagOff, tagLen;
    uint16_t classOff, classLen;
    uint16_t idOff, idLen;
  };
  std::vector<Entry> entries_;
  std::string chars_;
};

class CssParser {
 public:
  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // 7: `display: inline` is stored as CssDisplay::Inline (was folded into Block).
  // 8: rules carry direction (parsed before, but dropped by the cache), four packed border
  //    edges, and defined bits 17..29 (direction, page breaks, white-space, shade, borders).
  // 9: font-style / font-weight may hold Inherit (value 2), resolved at parse time (#4072).
  // 10: #id / descendant / child selectors (contextual rules follow the simple ones), one
  //     text byte after the borders (small caps, orphans, widows), font-size stored as u16
  //     hundredths + unit (3 bytes) instead of float + unit.
  // Bit 7 marks a cache written by a build with CROSSPOINT_EPUB_BLOCK_FONT_SIZES (every board
  // by default since v10), whose rules carry font-size; the two layouts never match each
  // other's version byte.
  static constexpr uint8_t CSS_CACHE_VERSION = 10 | (CROSSPOINT_EPUB_BLOCK_FONT_SIZES ? 0x80 : 0);

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const {
    return resolveStyle(tagName, classAttr, {}, nullptr);
  }

  /**
   * Full cascade: also #id rules and descendant / child rules matched against `ancestors`
   * (may be null when the book has no such rules or the caller does not track them).
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr, std::string_view idAttr,
                                      const CssAncestorStack* ancestors) const;

  /** True when descendant / child rules exist, i.e. callers should keep a CssAncestorStack. */
  [[nodiscard]] bool hasContextualRules() const { return !contextRules_.empty(); }

  /** Descendant / child rules held (also counted in ruleCount()). */
  [[nodiscard]] size_t contextualRuleCount() const { return contextRules_.size(); }

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return rulesBySelector_.empty() && contextRules_.empty(); }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return rulesBySelector_.size() + contextRules_.size(); }

  /**
   * Clear all loaded rules
   */
  void clear() {
    rulesBySelector_.clear();
    std::vector<ContextRule>().swap(contextRules_);  // release the capacity, not just the rules
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Check, without loading any rules, that a rules cache of the current
   * CSS_CACHE_VERSION exists under cachePath (i.e. loadFromCache() would not
   * reject it as stale).
   */
  static bool cacheIsCurrent(const std::string& cachePath);

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

 private:
  // Lookup key for a multi-piece selector. The pieces are hashed and compared
  // as if concatenated, so callers can look up composite keys without
  // materializing the concatenation in a scratch buffer. Constructed from a
  // braced list of any arity, e.g. `CompositeKey{tagName, ".", cls}` or
  // `CompositeKey{".", cls}`. The initializer_list's backing array lives for
  // the full expression, which covers the lifetime of the find() call.
  struct CompositeKey {
    std::initializer_list<std::string_view> pieces;
    CompositeKey(std::initializer_list<std::string_view> p) noexcept : pieces(p) {}
  };

  // ASCII-case-insensitive transparent hash/equal. Stored selectors and lookup
  // keys are compared without regard to case, so callers may insert and look up
  // using whatever case the CSS source or HTML element name happens to use.
  // Bodies live in CssParser.cpp so they can share the file-local asciiToLower.
  struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept;
    size_t operator()(const std::string& s) const noexcept;
    size_t operator()(CompositeKey k) const noexcept;
  };
  struct SvEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
    bool operator()(const std::string& a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, const std::string& b) const noexcept;
    bool operator()(const std::string& a, const std::string& b) const noexcept;
    bool operator()(CompositeKey a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, CompositeKey b) const noexcept;
  };

  // Storage: maps selector -> style properties. Hash/equal are case-insensitive.
  std::unordered_map<std::string, CssStyle, SvHash, SvEqual> rulesBySelector_;

  // Descendant / child rules, in source order. The selector is stored normalised: compounds
  // separated by a single ' ' (descendant) or '>' (child). ~132 bytes each on the C3 (a 24-byte
  // std::string, longer selectors also own a heap block, + CssStyle + specificity), held for a
  // section build like the simple rules; capped so a stylesheet built on them cannot take the
  // heap a build needs. Matching walks each rule right to left, with no allocation.
 public:
  static constexpr size_t MAX_CONTEXT_COMPOUNDS = 6;
#if defined(BOARD_HAS_PSRAM)
  static constexpr size_t MAX_CONTEXT_RULES = 256;
#else
  static constexpr size_t MAX_CONTEXT_RULES = 64;  // <= ~8.5 KB (+ long selector strings)
#endif

 private:
  struct ContextRule {
    std::string selector;
    CssStyle style;
    uint32_t specificity;
  };
  std::vector<ContextRule> contextRules_;
  void storeRule(std::string_view selector, const CssStyle& style);

  std::string cachePath;

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
