#pragma once

#include <ArduinoJson.h>
#include <WString.h>

#include <vector>

class CrossPointSettings;
class CrossPointState;
class WifiCredentialStore;
class KOReaderCredentialStore;
class BookFusionTokenStore;
class RecentBooksStore;
class ReadingStatsStore;
struct BookmarkEntry;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
// Fills `doc` with the on-disk form, so CrossPointSettings::saveToFile() can
// both write it and derive the settings.json compatibility copy from it.
void serializeSettings(const CrossPointSettings& s, JsonDocument& doc);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);
// Same, from an already-parsed document (an imported upstream settings.json
// after SettingsCompat::importUpstream()).
bool loadSettings(CrossPointSettings& s, const JsonDocument& doc, bool* needsResave = nullptr);

// CrossPointState
// Produces the on-disk form without writing it, so CrossPointState::saveToFile()
// can fingerprint exactly what would be written and skip an unchanged save.
void serializeState(const CrossPointState& s, String& json);
bool loadState(CrossPointState& s, const char* json);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave = nullptr);

// KOReaderCredentialStore
bool saveKOReader(const KOReaderCredentialStore& store, const char* path);
bool loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave = nullptr);

// BookFusionTokenStore
bool saveBookFusion(const BookFusionTokenStore& store, const char* path);
bool loadBookFusion(BookFusionTokenStore& store, const char* json);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooks(RecentBooksStore& store, const char* json);

// ReadingStatsStore
bool saveReadingStats(const ReadingStatsStore& store, const char* path);
bool loadReadingStats(ReadingStatsStore& store, const char* json);
bool loadReadingStatsFromFile(ReadingStatsStore& store, const char* path);

// Bookmarks
bool saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path);
bool loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json);

}  // namespace JsonSettingsIO
