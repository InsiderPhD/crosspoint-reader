#pragma once

#include <ArduinoJson.h>

// Translation between KatiePoint's settings document and the one upstream
// CrossPoint 2.x keeps in /.crosspoint/settings.json.
//
// KatiePoint's own settings live in /.crosspoint/katiepoint-settings.json, so
// another firmware rewriting settings.json cannot drop them. settings.json is
// still read once (an upstream user installing KatiePoint, or an older
// KatiePoint build) and is kept up to date with the settings both firmwares
// understand, so going back to CrossPoint carries the basics across.
//
// Pure JSON-to-JSON: no storage, no CrossPointSettings, so the tables can be
// checked on the host (test/settings_compat). Only keys whose meaning maps
// cleanly are translated; anything else stays at the receiving side's default
// (import) or is left as the other firmware last wrote it (export).
namespace SettingsCompat {

// True when the document was written by upstream CrossPoint 2.x (or by
// exportShared() below): it carries a key KatiePoint has never written.
bool isUpstreamDocument(JsonVariantConst doc);

// Upstream document -> KatiePoint keys and numbering. `out` is filled with only
// the keys that map; feed it to JsonSettingsIO::loadSettings(), whose own
// migrations then derive the rest (sleep theme, battery styles, header bar).
// `out` also gets "readerActionsMigrated": the per-button reader actions are
// written directly, so the legacy action migration must not overwrite them.
void importUpstream(JsonVariantConst upstream, JsonDocument& out);

// KatiePoint document (as JsonSettingsIO::saveSettings writes it) -> the shared
// keys in upstream names and numbering. Writes only into `out`; the caller
// merges the result over the existing settings.json.
void exportShared(JsonVariantConst katiepoint, JsonDocument& out);

}  // namespace SettingsCompat
