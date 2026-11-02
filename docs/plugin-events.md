# Plugin events and the book metadata sidecar

*X4 Pro / X4C only, like every plugin surface (`CROSSPOINT_SD_PLUGINS`), and
only while **Settings → System → Enable Plugins** is on.*

Two firmware surfaces that let SD plugins react to what happens on the reader
and attach service data to books, while keeping all service knowledge out of
the firmware. They follow upstream CrossPoint 2.0's contract
(`docs/plugin-events.md` there) and compose with the surfaces in
`sd-plugins.md`.

- **Events**: the firmware records a small whitelisted set of moments (a
  reading session ended, the device is going to sleep, ...) into a per-plugin
  queue on the SD card, and later replays them through HTTP request templates
  the plugin declared in its `device.json`. No browser, no plugin code on the
  device. Implemented in `src/util/PluginEvents.cpp`.
- **The sidecar**: a small JSON file next to a book (`<book path>.meta.json`)
  where plugins record fields like a service book id at download time. Its
  fields are available to event handlers as `{meta.*}` variables.

## Why delivery is deferred

Handlers need the network, and most events fire with WiFi down (leaving a
book, entering sleep). So emitting only appends one JSON line to the plugin's
outbox (`<plugin dir>/events.jsonl`), and the queue is drained through the
declared requests when the device is already online: whenever a WiFi
connection completes (the network picker, which File Transfer, OPDS and the
plugin catalogs all go through), and, bounded, on the way into sleep.
Delivery is at-least-once and in order; every queued line carries a unique
`id` (a per-boot nonce plus a counter) — dedupe on `{event.id}` server-side if
it matters.

Event subscriptions need a plugin folder name under 24 bytes and a full folder
path under 64; a plugin past either limit is logged and not subscribed. The
subscription table is rebuilt at boot, whenever the Plugins list opens, after
a catalog installs a bundle, and when the Plugins setting changes.

## The event whitelist

Event names are a compatibility promise; unknown names in a manifest are
ignored with a log line. Each is advertised in `/api/status`
`pluginCapabilities` as `events.<name>`.

| Event             | Fires when                                              | Variables |
| ----------------- | ------------------------------------------------------- | --------- |
| `reader.open`     | A book's first page is shown (EPUB / TXT / XTC)         | `book` (SD path) |
| `reader.exit`     | The reader is left (Home, another book, sleep)          | `book`, `percent` |
| `reader.session`  | An active-reading summary, flushed at exit and at sleep | see below |
| `book.downloaded` | A catalog download, or a book written by `/api/fetch` / `/api/plugin-fs`, completed | `path`, `title`, `plugin` |
| `sleep.enter`     | The device begins entering deep sleep                   | `book`, `percent` when sleeping out of a reader, else none |

`percent` is whole-book progress, 0-100, sent as a string like all event
variables. Opening a reader's menu or another child screen is not an exit.
`sleep.enter` carries the reader snapshot because sleeping straight out of a
book is the common flow; the reader's own `reader.exit` is queued after the
sleep-time drain and goes out on the next online drain.

Not sent on KatiePoint: upstream's `end_percent` (progress at the bottom of
the page).

### `reader.session`

| Variable | Meaning |
|---|---|
| `book` | SD path |
| `document` | binary KOSync partial-MD5, 32 lowercase hex characters |
| `start_time` | Unix time at the start of the first counted page |
| `end_time` | exactly `start_time + duration_seconds` |
| `duration_seconds` | sum of counted page times |
| `start_progress_bp` / `end_progress_bp` | whole-book progress in basis points (0..10000) |
| `progress_scale` | literal `10000` |

Only ordinary forward page turns count (the next page, or the first page of
the next chapter after the last). A page contributes when it stayed on screen
at least 2 seconds, capped at 1800 seconds; reverse turns and jumps are
excluded and restart the timer, while a repaint of the same page (a menu
closing, the status bar clock) does not. At least 10 counted seconds and a set
clock (RTC or NTP) are required. Progress has whole-percent resolution here
(steps of 100 basis points). The document hash is only computed when a summary
will actually be sent.

## Declaring handlers in `device.json`

```json
"events": {
  "reader.exit": {
    "request": {
      "method": "POST",
      "url": "{cfg.server}/api/progress",
      "headers": { "Authorization": "Bearer {token}" },
      "body": "{\"book\":\"{event.book}\",\"pct\":{event.percent},\"id\":\"{meta.service_id}\",\"ts\":{event.ts}}"
    },
    "require_meta": "service_id"
  },
  "sleep.enter": {
    "download": { "url": "{cfg.server}/daily.bmp", "dest": "/sleep.bmp" }
  }
}
```

One handler per event, either:

- **`request`** — `url`, `method` (default `POST`), `headers`, `body`. Any 2xx
  counts as delivered; the body is discarded (8KB cap).
- **`download`** — `url`, `method` (default `GET`), `headers`, `dest`. The
  response streams to `dest.part` and replaces `dest` only on a 2xx (1MB cap),
  so an error page never overwrites the existing file. `dest` must be absolute,
  without `..`, and never a credential store. A book file outside hidden
  folders marks the Library index stale.

Optional per handler:

- **`require_meta`** — a sidecar key. The event is queued for this handler only
  when the event's book (`book`, or `path` for `book.downloaded`) has a sidecar
  where that key holds a non-empty string or a whole number. An event without
  a book is not queued. A `require_meta` that is not a string makes the
  handler ignored, so a wrong type cannot turn the filter off.
- **`connect`** — `true` lets the handler's events be delivered at sleep entry,
  bringing WiFi up if needed. `sleep.enter` handlers have it implicitly.
- **`toast`** — accepted for upstream compatibility but not shown: KatiePoint
  delivers events at moments without a screen to spare.

## sleep.enter delivery

At sleep entry the firmware queues `reader.session` (if a book is open) and
`sleep.enter`, then, if any handler with `connect` has something queued: when
WiFi is not already up, it joins the last-connected saved network (battery at
least 20%, 10-second deadline), delivers only those opted-in events (10-second
budget, oldest first), and turns WiFi back off. Sleep is never blocked beyond
those bounds — a failed join or delivery leaves the events queued for the next
drain. The sleep image is drawn after this, so a `download` to the sleep image
path shows on that very sleep.

Auth reuses the catalog vocabulary of the same `device.json`: `{token}` from
the declared token file, and on a 401/403 a `"password"` auth section mints a
fresh token and retries once.

## Substitution variables

Templates in `url`, `headers`, `body` and `dest` substitute `{token}`,
`{cfg.KEY}`, `{event.NAME}` (the event's variables), `{event.ts}` (Unix time
when it fired; 0 without a set clock), `{event.id}`, and `{meta.KEY}` (the
book's sidecar). Values are inserted as they are; the URL is escaped for
spaces, control characters and non-ASCII before sending. Unmatched variables
stay literal.

## The book metadata sidecar

A flat JSON file next to the book, `<book path>.meta.json`:

```json
{ "service_id": "36835", "source": "myservice" }
```

Written by the catalog's `download.sidecar`, by browser plugins via
`/api/plugin-fs`, or by hand. All plugins share one sidecar per book: namespace
your keys. Fields are flat strings or numbers (2KB read cap); `{meta.*}` sees
strings and whole numbers.

## Delivery semantics and limits

- At-least-once, in order per plugin: a drain stops at a plugin's first failed
  delivery; delivered events are removed.
- A corrupt queue line, or one whose event no longer has a handler, counts as
  delivered, so a manifest edit cannot wedge the queue.

| Limit | Value |
|---|---|
| Outbox | 4KB per plugin, then dropped wholesale |
| Event line | 512 bytes |
| Request response | 8KB (discarded) |
| Download | 1MB, streamed to SD |
| Subscribed plugins | 8 |
| Handlers per drain | 4 on a WiFi connection (15s budget); at sleep, opted-in events only (10s budget) |
| Sidecar / config / token files | 2KB each |
