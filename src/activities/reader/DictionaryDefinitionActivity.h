#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WordRef.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DictionaryLookup;

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
//
// Recursive lookup (upstream #4063, our implementation): Confirm selects a word
// on the page and looks it up in the same dictionary, replacing the definition
// in place; Back returns to the previous definition and page; holding Confirm
// closes the whole chain. Only ONE definition is ever resident: the current one
// is released before the next lookup runs, and Back re-reads the previous one
// from the SD card. What is kept per level is the query that produced it and
// its page — see History.
class DictionaryDefinitionActivity final : public Activity {
 public:
  // lookup/query enable nested lookups: the reader's dictionary session (it
  // outlives this activity, which is a reader sub-screen) and the word that
  // produced this definition, replayed when the user comes back to it. Without
  // them the screen is a plain viewer.
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false,
                                        DictionaryLookup* lookup = nullptr, std::string query = {})
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        lookup(lookup),
        query(std::move(query)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // A reader sub-screen: keeps Bluetooth alive across the lookup on boards where
  // the two can coexist, so an X4 Pro page-turner does not drop while a
  // definition is open. (On the C3 boards a lookup requires Bluetooth off
  // already — see EpubReaderActivity::openDictionaryLookup.)
  bool isReaderActivity() const override { return true; }
  // Taps page the definition here, so the global tap-is-Confirm injection must
  // stay off — it would otherwise fire alongside the page turn.
  bool handlesDirectTouch() const override { return true; }

 private:
  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  // bold: the line began with DICTIONARY_PLAIN_HEADWORD_MARKER (an entry's
  // headword label, or a heading in the HTML fallback); still 8 bytes.
  struct Line {
    uint32_t start;
    uint16_t len;
    bool bold;
  };

  // Usable body-text area between the header and the button hints.
  struct BodyArea {
    int width;
    int height;
  };

  // The definitions behind the current one, newest last. LIFO, so each query
  // is appended to one fixed pool and popped off its end. Per level: a 4-byte
  // entry plus the query's bytes (+NUL) — typically ~12 bytes, at worst
  // MAX_PHRASE_BYTES for a long phrase. Capped at MAX_DEPTH levels / POOL_BYTES
  // of queries (~580 bytes in all), allocated on the first nested lookup and
  // freed with the activity. Never a definition: Back re-reads it from SD.
  struct History {
    static constexpr uint8_t MAX_DEPTH = 16;
    static constexpr uint16_t POOL_BYTES = 512;
    struct Entry {
      uint16_t queryOffset;
      uint16_t page;
    };
    Entry entries[MAX_DEPTH] = {};
    char pool[POOL_BYTES] = {};
    uint16_t poolUsed = 0;
    uint8_t depth = 0;

    bool push(const std::string& query, int page);
    const char* topQuery() const { return pool + entries[depth - 1].queryOffset; }
    int topPage() const { return entries[depth - 1].page; }
    void pop() {
      poolUsed = entries[depth - 1].queryOffset;
      depth--;
    }
  };

  BodyArea bodyArea() const;
  // Body origin on screen (left edge of the text, top of the first line).
  void bodyOrigin(int& x, int& y) const;
  // Lay `definition` out for display (styled pages or wrapped spans).
  void prepareDefinition();
  bool layoutHtmlPages();
  void wrapText();
  // Drop the current definition and its layout, returning the heap.
  void releaseDefinition();
  int measureSpan(int fontId, const char* text, size_t len, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawBody(int fontId, int x, int startY) const;

  // Nested lookup.
  bool canLookUp() const { return lookup != nullptr && !query.empty(); }
  // The selectable words of the current page, built when selection starts.
  bool buildPageWords();
  void startSelection();
  void endSelection();
  void lookUpWord(const std::string& word);
  void goBack();
  // Replace the current definition with the lookup of q. False (definition
  // released) when it is not found or fails; *message names why.
  bool loadQuery(const char* q, StrId* message);
  void showMessage(StrId message);
  void moveCursorLine(int direction);

  std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  bool htmlDefinition;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;

  DictionaryLookup* lookup;
  std::string query;
  std::unique_ptr<History> history;
  // Word selection on the current page; words is empty when not selecting.
  bool selecting = false;
  WordList pageWords;
  int cursor = 0;
  // "Looking up..." is painted over the definition before a nested lookup.
  bool busy = false;
  // A short popup over the definition ("Not found", "History full").
  bool messageVisible = false;
  StrId message = StrId::STR_DICT_NOT_FOUND;
  unsigned long messageTime = 0;
};
