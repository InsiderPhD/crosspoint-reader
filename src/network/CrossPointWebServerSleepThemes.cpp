// Sleep-theme routes: the compiler page and the live-value list it compiles
// and previews against. Uploading the compiled files reuses /mkdir and
// /upload. See src/sleeptheme/ and docs/file-formats.md.
#include <ArduinoJson.h>
#include <DevicePolicy.h>
#include <HalDisplay.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "CrossPointWebServer.h"
#include "html/SleepThemesPageHtml.generated.h"
#include "sleeptheme/SleepThemeFormat.h"
#include "sleeptheme/SleepThemePluginVars.h"
#include "sleeptheme/SleepThemeRenderer.h"
#include "sleeptheme/SleepThemeVars.h"

void CrossPointWebServer::handleSleepThemesPage() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", SleepThemesPageHtml, sizeof(SleepThemesPageHtml));
  LOG_DBG("WEB", "Served sleep themes page");
}

// GET /api/sleep-theme/vars
// {"panelWidth":800,"panelHeight":480,"themesDir":"/sleep-themes","formatVersion":1,
//  "plugins":true (this board runs SD plugins),"active":"name",
//  "limits":{...},"vars":[{"name":"stats.total_hours","kind":"number","value":"61","raw":61},...]}
// Values are the user's real ones right now, so the page previews with them.
// Streamed one var at a time to keep the response off the heap.
void CrossPointWebServer::handleSleepThemeVars() const {
  using SleepThemeVars::Id;
  using SleepThemeVars::Kind;

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"panelWidth\":%u,\"panelHeight\":%u,\"themesDir\":\"%s\",\"formatVersion\":%u,\"plugins\":%s,"
           "\"limits\":{\"elements\":%u,\"fonts\":%u,\"strings\":%u},\"active\":",
           display.getDisplayWidth(), display.getDisplayHeight(), SleepThemeRenderer::THEMES_DIR,
           SleepThemeFormat::VERSION, CROSSPOINT_SD_PLUGINS ? "true" : "false", SleepThemeFormat::MAX_ELEMENTS,
           SleepThemeFormat::MAX_FONTS, SleepThemeFormat::MAX_STRING_TABLE);
  server->sendContent(buf);
  {
    JsonDocument doc;
    doc.set(SETTINGS.sleepTheme);
    serializeJson(doc, buf, sizeof(buf));
    server->sendContent(buf);
  }
  server->sendContent(",\"vars\":[");

  SleepThemeVars::beginFrame();
  char value[SleepThemeVars::VALUE_LEN];
  for (size_t i = 0; i < static_cast<size_t>(Id::Count); i++) {
    const auto id = static_cast<Id>(i);
    const auto& info = SleepThemeVars::info(id);
    SleepThemeVars::format(id, nullptr, 0, value, sizeof(value));
    const char* kind = "text";
    switch (info.kind) {
      case Kind::Number:
        kind = "number";
        break;
      case Kind::Timestamp:
        kind = "timestamp";
        break;
      case Kind::Image:
        kind = "image";
        break;
      case Kind::Text:
        break;
    }
    JsonDocument doc;
    doc["name"] = info.name;
    doc["kind"] = kind;
    doc["value"] = value;
    // Raw value too, so the page can preview data-format patterns.
    int64_t number = 0;
    uint32_t epoch = 0;
    if (info.kind == Kind::Number && SleepThemeVars::number(id, number)) doc["raw"] = number;
    if (info.kind == Kind::Timestamp && SleepThemeVars::timestamp(id, epoch)) doc["raw"] = epoch;
    if (i > 0) server->sendContent(",");
    serializeJson(doc, buf, sizeof(buf));
    server->sendContent(buf);
  }
  // Values installed plugins publish right now (plugin.<name>.<key>). The page
  // accepts any well-formed plugin name, so these only fill in the preview.
  // Serialized to a string, not buf: a plugin value can be up to its 2KB file,
  // and a truncated serialization would be broken JSON. Only the PSRAM boards
  // have plugins, so this never runs on the C3.
  SleepThemePluginVars::forEach(
      [](void* ctx, const char* name, const char* value) {
        auto* web = static_cast<WebServer*>(ctx);
        JsonDocument doc;
        doc["name"] = name;
        doc["kind"] = "text";
        doc["value"] = value;
        std::string json;
        serializeJson(doc, json);
        web->sendContent(",");
        web->sendContent(json.c_str());
      },
      server.get());
  SleepThemeVars::endFrame();
  server->sendContent("]}");
  server->sendContent("");
}
