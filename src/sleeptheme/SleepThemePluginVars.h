#pragma once

#include <cstddef>

// Sleep-theme values published by SD plugins: {{plugin.<name>.<key>}}.
//
// A plugin writes a flat JSON object of strings or whole numbers to
// sleep-vars.json in its own folder (through /api/plugin-fs, a device.json
// event download, or by hand); a theme shows a key with {{plugin.<name>.<key>}}.
// The firmware only reads the file when it draws the screen. The plugin decides
// when the values change (daily, per sync, per sleep). <name> is the plugin's
// folder name with every '-' written as '_' (theme names cannot hold '-'). Both
// segments are [A-Za-z0-9_]+.
//
// Plugin values are always text. A missing plugin, file or key reads as empty,
// as does every plugin value on boards without SD plugins (the C3 boards) or
// while Settings → System → Enable Plugins is off, so data-hide-if-empty keeps
// a theme usable everywhere.
namespace SleepThemePluginVars {

inline constexpr char FILE_NAME[] = "sleep-vars.json";

// Optional rotation beside FILE_NAME: one flat JSON object per line (JSON
// Lines), one line per day. The line for today (local day number modulo the
// count of non-blank lines) is laid over FILE_NAME's values, so a plugin can
// ship a year of words or quotes that change daily with no network and no
// plugin code running. Only the chosen line is held in memory (each line at
// most DAILY_LINE_MAX bytes). Skipped while the clock is unknown: FILE_NAME's
// values then show alone, so a plugin can put a fallback there.
inline constexpr char DAILY_FILE_NAME[] = "sleep-vars-daily.jsonl";
inline constexpr size_t DAILY_LINE_MAX = 2048;

// True when name (not NUL-terminated) has the shape plugin.<name>.<key>. Built
// on every board, so a theme using plugin values never logs as unknown.
bool isPluginVar(const char* name, size_t nameLen);

// Writes the value of a name isPluginVar() accepted into out, always
// NUL-terminated, truncated to fit. Empty when it is unknown right now.
void lookup(const char* name, size_t nameLen, char* out, size_t outSize);

// Called from SleepThemeVars::beginFrame()/endFrame(): between them, each
// plugin's file is read once instead of once per element. Outside a frame
// lookup() still works, reading the file every time.
void beginFrame();
void endFrame();

// Every value of every installed plugin that has a sleep-vars.json, as
// fn(ctx, "plugin.<name>.<key>", value), for the theme compiler's value list.
// Calls nothing on boards without SD plugins or while plugins are off.
void forEach(void (*fn)(void* ctx, const char* name, const char* value), void* ctx);

}  // namespace SleepThemePluginVars
