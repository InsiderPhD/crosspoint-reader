#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
  uint8_t buttonHints = 0;
  uint8_t autosyncMode = 0;
  // Frontlight levels as cycled in the menu (already driven live on the
  // hardware); the reader persists them on menu exit when they changed.
  uint8_t frontlightBrightness = 0;
  uint8_t frontlightWarmth = 50;
};

// Returned by FrontlightBrightnessActivity. The picker previews levels live but
// never persists; the caller decides when the chosen level reaches SPIFFS.
struct FrontlightResult {
  uint8_t brightness = 0;
};

// Returned by ReaderActionSelectActivity: the reader action picked for one
// Reader Controls row. Not persisted by the picker — the caller writes it.
struct ReaderActionResult {
  uint8_t action = 0;
};

struct ChapterResult {
  int spineIndex = 0;
};

struct PercentResult {
  int percent = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;
  // BookFusion stores positions as a percent within the spine item, not a
  // literal page number. When the result comes from BookFusion, the reader
  // uses this fraction (via pendingSpineProgress / pendingPercentJump) to
  // resolve the destination page once the new section's pageCount is known.
  // < 0 means "use page" (KOReader path).
  float intraSpineProgress = -1.0f;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct BookContextResult {
  enum class Action { MarkAsRead, RemoveFromRecents } action;
};

struct FilePathResult {
  std::string path;
};

// Returned by ClipSelectionActivity when the user confirms a selection. Carries the cleaned
// clipping text plus the geometry/anchors needed to persist and later re-locate it.
struct ClippingResult {
  std::string text;
  int fromWordIdx = -1;
  int toWordIdx = -1;
  uint16_t sectionPage = 0;
  uint16_t endSectionPage = 0;
  uint16_t sectionPageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  std::string startText;
  std::string endText;
  std::string beforeStartText;
  std::string afterEndText;
  std::string midText;
  uint16_t wordCount = 0;
};

// Returned by EpubReaderClippingListActivity when the user picks a clipping to jump to.
struct ClippingJumpResult {
  uint16_t spineIndex = 0;
  uint16_t page = 0;
  uint16_t pageCount = 1;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t clippingIndex = UINT16_MAX;
};

// Returned by ClipSelectionActivity in single-word mode: one word picked off the
// page, with none of the surrounding context a clipping needs. Used by the
// reader's dictionary lookup, which wants the word and nothing else.
struct WordPickResult {
  std::string word;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, FrontlightResult, ReaderActionResult,
                 ChapterResult, PercentResult, PageResult, SyncResult, NetworkModeResult, FootnoteResult,
                 BookContextResult, FilePathResult, ClippingResult, ClippingJumpResult, WordPickResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
