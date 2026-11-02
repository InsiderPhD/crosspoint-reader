#pragma once

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
//
// headingMarker (optional): when non-zero, an <h1>-<h6> becomes a label line of
// its own that starts with this byte, so the dictionary viewer can draw it in
// bold (see DICTIONARY_PLAIN_HEADWORD_MARKER). Zero keeps a heading as a plain
// paragraph.
std::string htmlToPlainText(const std::string& html, char headingMarker = '\0');
