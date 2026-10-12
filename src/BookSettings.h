#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include "CrossPointSettings.h"

// Per-book reader settings: an overlay on the SETTINGS singleton that lasts for
// as long as one EPUB is open.
//
// A book may override any of the layout fields in kFields. The overrides live
// in <book cache dir>/book_settings.bin, so they follow a rename (the cache
// directory moves with the book, see BookMoveUtils) and go when the cache does.
// begin() reads that file once and writes the overridden values straight into
// SETTINGS, so every existing reader of SETTINGS (layout, section cache key,
// font resolver) picks them up with no change; end() puts the global values
// back. Nothing here allocates: the state is one fixed struct in .bss.
//
// Global settings are never written to disk with a book's values in them:
// JsonSettingsIO asks globalValue()/globalString() for every field it saves, and
// while an overlay is active those return the remembered global value for an
// overridden field. That is done at serialisation rather than by swapping the
// fields back and forth around the save, because the render task reads SETTINGS
// concurrently and must never see the globals mid-page.
namespace BookSettings {

struct Field {
  uint8_t CrossPointSettings::* member;
  uint8_t min;
  uint8_t max;  // inclusive
};

// The index of a field is its bit in the override mask and its byte in the
// file. PERSISTED -- append only, never reorder or remove. Bit 0 (font family)
// also carries sdFontFamilyName, which overrides the built-in family when set.
inline constexpr Field kFields[] = {
    {&CrossPointSettings::fontFamily, 0, CrossPointSettings::FONT_FAMILY_COUNT - 1},
    {&CrossPointSettings::fontSize, 0, CrossPointSettings::FONT_SIZE_COUNT - 1},
    {&CrossPointSettings::lineSpacing, 0, CrossPointSettings::LINE_COMPRESSION_COUNT - 1},
    {&CrossPointSettings::wordSpacing, CrossPointSettings::WORD_SPACING_MIN, CrossPointSettings::WORD_SPACING_MAX},
    {&CrossPointSettings::characterSpacing, 0, CrossPointSettings::CHARACTER_SPACING_COUNT - 1},
    {&CrossPointSettings::paragraphAlignment, 0, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1},
    {&CrossPointSettings::screenMargin, 5, 40},
    {&CrossPointSettings::extraParagraphSpacing, 0, 1},
    {&CrossPointSettings::paragraphIndent, 0, TextLayoutOptions::PARAGRAPH_INDENT_COUNT - 1},
    {&CrossPointSettings::bionicReading, 0, 1},
    {&CrossPointSettings::textAntiAliasing, 0, 1},
    {&CrossPointSettings::hyphenationEnabled, 0, 1},
    {&CrossPointSettings::imageRendering, 0, CrossPointSettings::IMAGE_RENDERING_COUNT - 1},
};
inline constexpr size_t kFieldCount = std::size(kFields);

// Starts the overlay for the book whose cache directory is cacheDir and applies
// any saved overrides to SETTINGS. Ends a previous overlay first. Call before
// the book is laid out (and before its font is loaded).
void begin(const std::string& cacheDir);

// Restores the global values and forgets the book. Safe to call when inactive.
void end();

// True between begin() and end().
bool isActive();
// True when the open book overrides at least one field.
bool hasOverrides();
// True when the open book overrides this field.
bool isOverridden(uint8_t CrossPointSettings::* member);

// Call BEFORE changing a field on the book's behalf: remembers the global value
// and marks the field overridden. False when no overlay is active or the field
// cannot be overridden.
bool beginOverride(uint8_t CrossPointSettings::* member);

// Writes the overrides to the sidecar, or removes it when there are none.
bool save();

// If the open book overrides this field, saves the live value as the book's and
// returns true; the caller should then skip the global save.
bool saveIfOverridden(uint8_t CrossPointSettings::* member);

// "Use Global Settings": puts every global value back, clears the overrides and
// removes the sidecar. The overlay stays active (and empty).
void reset();

// Editing the GLOBAL settings from inside a book. suspend() shows the global
// values in SETTINGS so the editor edits (and saves) those; resume() re-reads
// them as the new globals and re-applies the book's overrides on top.
void suspend();
void resume();

// For JsonSettingsIO: the value to persist for a field of SETTINGS.
uint8_t globalValue(const CrossPointSettings& s, uint8_t CrossPointSettings::* member);
const char* globalString(const CrossPointSettings& s, const char* field);

}  // namespace BookSettings
