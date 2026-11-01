#pragma once

#include <cstddef>

// Expressions inside a sleep-theme template's {{ }}:
//   {{stats.streak == 1 ? "day" : "days"}}
//   {{round(stats.week_minutes / 60)}}h
//   {{book.title || "Nothing open"}}
// Numbers, "strings" (or 'strings'), value names, + - * / %, == != < <= > >=,
// && || ! (JavaScript-style: || and && return an operand), ?:, parentheses,
// and round floor ceil abs min max upper lower pad plural date.
// The browser compiler checks and previews them with a matching evaluator
// (SleepThemesPage.html); keep the two in step.
namespace SleepThemeExpr {

// True for the plain "name" / "name:format" form, which SleepThemeVars
// formats itself.
bool isPlainValue(const char* src, size_t len);

// Evaluates src (the text between {{ and }}) into out, always NUL-terminated.
// present is false when the result is empty, which hide-if-empty uses. False
// on a syntax error (logged); out is then empty.
bool evaluate(const char* src, size_t len, char* out, size_t outSize, bool& present);

}  // namespace SleepThemeExpr
