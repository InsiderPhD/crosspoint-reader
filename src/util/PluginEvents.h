#pragma once

#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy
#include <cstddef>
#include <cstdint>

#include "ScreenshotInfo.h"

// Firmware events for SD plugins (docs/plugin-events.md), ported from upstream
// CrossPoint 2.0 / CrossInk's PluginEvents.
//
// The firmware records a small whitelisted set of moments; a plugin's
// device.json subscribes with an "events" section whose handlers are the same
// declarative request objects the catalog vocabulary uses:
//
//   "events": {
//     "reader.exit": { "request": { "method": "POST", "url": "https://...",
//                                   "body": "{\"book\":\"{event.book}\"}" } },
//     "sleep.enter": { "download": { "url": "https://.../daily.bmp", "dest": "/sleep.bmp" } }
//   }
//
// Delivery is deferred: emit() appends one JSON line to the plugin's SD outbox
// (<plugin dir>/events.jsonl) and nothing more; drain() replays the queue
// through the declared requests when the device is already online (a WiFi
// connection completing) and, bounded, at sleep entry. At-least-once, in order.
namespace pluginevents {

enum class Event : uint8_t {
  ReaderOpen,      // "reader.open"      vars: book
  ReaderExit,      // "reader.exit"      vars: book, percent
  ReaderSession,   // "reader.session"   vars: book, document, active-session summary
  BookDownloaded,  // "book.downloaded"  vars: path, title, plugin
  SleepEnter,      // "sleep.enter"      vars: book, percent when sleeping from a reader, else none
  COUNT
};

struct Var {
  const char* key;    // name inside {event.*}, e.g. "book"
  const char* value;  // NUL-terminated UTF-8
};

// Rebuilds the subscription table from every installed plugin's device.json.
// Also runs lazily on first use and whenever the Plugins setting changes.
void refreshSubscriptions();

// True when at least one plugin subscribes to `e`.
bool anySubscriber(Event e);

// Bit per Event the named plugin subscribes to (0 = none), so the picker can
// disclose what activity each plugin is sent.
uint8_t subscriptionMask(const char* plugin);

// Appends the event to each subscribing plugin's outbox (no-op without one).
void emit(Event e, const Var* vars, size_t varCount);

// Replays queued events through their handlers when WiFi is connected (a
// no-op otherwise); runs synchronously on the caller. At most `maxEvents`
// handlers run, within about `budgetMs`; the backlog waits for the next drain.
void drain(size_t maxEvents = 4, uint32_t budgetMs = 15000);

// --- Reader tracking (reader.open / reader.exit / reader.session) ----------
// Render task, after a reader painted a page: records scalars only (no SD, no
// allocation). `onScreen` false = the reader is no longer anywhere on the
// activity stack. Cheap no-op unless a plugin subscribes to a reader event.
void noteReaderRender(const ScreenshotInfo& info, bool onScreen);
// True when a plugin subscribes to a reader event (an atomic read, safe on the
// render task); lets callers skip building the snapshot.
bool readerEventsWanted();
// Main loop: turns the latest snapshot into reader.open / reader.session /
// reader.exit events (SD appends). Must not run while the render task paints.
void pollReader();

// Sleep entry, before the sleep screen: queues reader.session (if any) and
// sleep.enter, then, when a handler that opted into "connect" has something
// queued, joins the saved network (battery >= 20%, 10s deadline) and drains
// those events; finally queues reader.exit for the book being left. Never
// blocks sleep on the network beyond those bounds.
void prepareSleep(bool allowNetwork);

}  // namespace pluginevents

#endif  // CROSSPOINT_SD_PLUGINS
