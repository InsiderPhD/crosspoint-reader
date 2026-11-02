# SD-card plugins

**Which boards:** the plugin surfaces are built only for the PSRAM boards (X4
Pro, X4C) — `CROSSPOINT_SD_PLUGINS` in `lib/DevicePolicy/DevicePolicy.h`. The
X3/X4 (ESP32-C3) cannot hold a 32KB relay body, the 48KB API response caps and
a kept-alive TLS session next to WiFi; on the S3 boards every allocation of
4KB or more is served from PSRAM, so none of it touches internal DRAM. The C3
image still serves `/plugins` as a one-line explanation so the web nav tab
never 404s. Plugins are an optional extra: the native OPDS, BookFusion and
dictionary-download features stay on every board and are not affected.

**Plugins the firmware refuses.** Services this firmware implements natively
are not accepted as plugins: a folder whose name starts with `bookfusion` or
`dictionar` (case-insensitive; the sd-plugins repository's `bookfusion` and
`dictionaries` folders) is ignored by discovery, never served, and the plugin
store cannot install it (`/api/plugin-fs` and `/api/fetch` answer 403). The
list is `PluginLocations::kNativeFeaturePrefixes`. Use the built-in BookFusion
(File Transfer / Settings → System → BookFusion Sync) and the dictionary
downloader (Settings → Dictionary) instead; two implementations of the same
account would fight over tokens and progress.

**The Plugins switch.** **Settings → System → Enable Plugins**
(`pluginsEnabled`, the same key and meaning as upstream CrossPoint 2.0's
global toggle, #3877) gates every plugin surface: while it is off, discovery
lists nothing, `/plugin` and `/api/plugins` find no plugin, and `/api/relay`,
`/api/fetch`, `/api/crypto`, `/api/plugin-fs` and the job queue answer
`403 plugin system disabled`. Turning it off never deletes or renames plugin
files. New installs default to off; a settings file without the key (an
update from an older build, or an older upstream `settings.json`) gets it
switched on when plugin folders are already on the card, so an update never
silently disables a working setup. The key is shared with upstream's
`settings.json` both ways.

**Where to find them on the reader:** **Settings → System → Plugins**, and
**Home → File Transfer → Plugins** (that row appears once a plugin with a
`device.json` is on the card). In the browser: the **Plugins** tab of the
device's web pages.

KatiePoint keeps service integrations off the firmware. A plugin is a folder on
the SD card; the firmware provides three generic, scheme-neutral surfaces that
plugins compose. Adding or updating a plugin never requires a firmware build,
and the firmware carries no vendor names, URLs, or file-format knowledge.

```
<root>/<name>/
    manifest.json     web UI card metadata (optional): title, description, mount
    plugin.js         browser-side plugin (optional)
    device.json       on-device catalog screen (optional): title, description, ...
    README.md         usage instructions, shown on-device (optional)
    ...assets
```

**Discovery and the on-device list.** Every plugin folder (anything holding a
`manifest.json`, `plugin.js`, or `device.json`) appears under **Settings →
System → Plugins** on the reader, showing its `title` and one-line
`description`. Selecting a plugin that ships a `device.json` opens its
on-device catalog directly. Selecting a browser-only plugin (no `device.json`)
opens an info screen instead: the description and the plugin's `README.md` as
scrollable usage instructions (`PluginInfoActivity`), so a web-only plugin is
still listed and can explain how to use it from the web UI. A README over
24 KB is skipped; a plugin with neither README nor description shows a hint
pointing at the web Plugins tab. `title`/`description` are read from
`manifest.json`, with `device.json` overriding when present.

**Uninstalling.** Holding Confirm for one second on a picker row (or a touch
long-press on the row) asks to uninstall that plugin; confirming deletes its
whole folder (`<root>/<name>`, only ever a direct child of a plugin root),
including its token and config files. Because the picker uses the hold,
Confirm opens a row on release there.

`<root>` is any of `/.crosspoint/plugins`, `/plugins`, or `/.plugins` — the
first two-dot-free options exist so plugins are easy to copy onto the card
from a computer. All three roots are scanned; on a name collision the earlier
root in that order wins.

A plugin can ship any combination: `plugin.js` alone (web-only),
`device.json` alone (on-device only), or both (e.g. sign in from either side,
browse on the reader).

The plugin sources and examples live in the separate `sd-plugins` repository,
which also documents the browser-side `plugin.js` API. This document covers the
firmware surfaces: the job queue and the `device.json` on-device screens.

## Surface 1: browser plugins (`plugin.js`)

JS loaded into the device's **Plugins** web page, backed by generic device
endpoints (`/api/relay`, `/api/crypto`, `/api/fetch`, `/api/plugin-fs`).
`/api/crypto` is a stateless set of wolfCrypt primitives, base64 in and out:
`random`, `sha1`, `sha256`, `hmac` (SHA-256), `aesenc`/`aesdec` (AES-128-CBC,
PKCS#7 padded on encrypt), `keygen` (RSA-1024, SPKI + PKCS#8), `pubencrypt`
(RSAES-PKCS1-v1_5 to an X.509 certificate), `sign` (raw RSA-1024 over a
20-byte hash) and `pkcs12` (bundle → PKCS#8 key + certificate). It exists
because a page served over plain HTTP has no `crypto.subtle`. See the
sd-plugins repository for the JS contract.

The host script itself (discovery, `plugin.js` loading, the `api` object and
the job poller) is one shared asset, `GET /plugin-host.js` (also at
`/js/plugin-host.js`, upstream's path): the Plugins page and `/plugins-run`
load it, and a page a plugin brings can too.

Plugin files are served by `GET /plugin?name=&file=` with an `ETag` (MD5 of
the file's content) and `Cache-Control: no-cache`: the browser keeps the file
and revalidates it on every page load, and an unchanged file answers
`304 Not Modified` instead of being sent again.

## Surface 2: the plugin job queue (external automation)

External systems (a companion app, a script) can trigger plugin actions
without a human clicking the web UI. The firmware stores small opaque
`{plugin, action, args}` JSON blobs in a fixed 6-slot pool; it never interprets
them. Any open page hosting the plugin — the **Plugins** tab, or the headless
runner at `GET /plugins-run` — claims jobs, executes the plugin's registered
handler in the browser context, and posts the result back.

| Endpoint | Purpose |
|---|---|
| `POST /api/plugin-jobs` | enqueue `{plugin, action, args?}` → `{id}` (503 when the pool is full) |
| `GET /api/plugin-jobs/claim?plugin=` | executor claims the next pending job |
| `POST /api/plugin-jobs/complete` | executor posts `{id, ok, result?}` |
| `GET /api/plugin-jobs/status?id=` | caller polls: `{id, state, result}`; states: `pending`, `running`, `done`, `error`, `unknown` (recycled) |
| `GET /plugins-run` | headless page that loads every plugin (UI hidden) and executes jobs while open |

Flow for an external system:

```sh
# 1. upload whatever the action needs (e.g. a file, via /upload)
# 2. enqueue
curl -X POST http://crosspoint.local/api/plugin-jobs \
  -d '{"plugin":"<name>","action":"<action>","args":{"path":"/folder/file"}}'
# -> {"id":7}
# 3. open http://crosspoint.local/plugins-run in a browser tab or hidden
#    webview (jobs only execute while a hosting page is open)
# 4. poll
curl "http://crosspoint.local/api/plugin-jobs/status?id=7"
# -> {"id":7,"state":"done","result":{...}}
```

Limits: plugin/action names < 24 chars, args and result < 192 bytes of JSON
each. A job claimed by a runner that dies is reclaimable after 10 minutes.
Results persist only until their slot is recycled — poll promptly.

Plugins register handlers in `plugin.js`:

```js
api.registerAction('myaction', async (args) => {
  // runs in the hosting page; throw -> state "error" with {error: message}
  return { anything: 'small' };  // -> state "done" with this result
});
```

## Surface 3: on-device catalog screens (`device.json`)

A declarative manifest the firmware's generic `PluginCatalogActivity` renders
under **Settings → System → Plugins**. It expresses "authenticated JSON
catalog: sign in, browse, download, sidecar" — enough for most book services —
without any code running on the device. Anything beyond this vocabulary
belongs in `plugin.js`.

The firmware pieces (service-agnostic, in `src/activities/plugins/`):
`PluginCatalogActivity`, one activity that opens as a picker over the
installed plugins (title + description rows) and morphs into the
manifest-driven browse / download / sign-in flow for the picked `device.json`
plugin; and `PluginInfoActivity`, the description + README screen the picker
pushes for a browser-only plugin (`plugin.js` without a `device.json`), whose
UI lives in the web interface. `discoverPlugins()` rescans the plugin folders
each time the picker opens; nothing stays resident.

### Firmware identity and plugin capabilities

Catalogs decide what to offer from what the firmware says it can do, not from
a version number. KatiePoint keeps upstream CrossPoint 2.0's plugin contract,
so it reports the same family (`src/util/FirmwareIdentity.h`):

| Value | KatiePoint (X4 Pro / X4C) | Purpose |
|---|---|---|
| `firmware_family` | `crosspoint` | broad compatibility family |
| `plugin_api` | `1` | numeric plugin contract level |
| `plugin_capabilities` | comma-separated names below | fine-grained feature list |

A capability is listed only once its behaviour is implemented here, so a
catalog entry that `requires` something this firmware lacks is never offered.
Boards without plugin support (X3 / X4) report API `0` and no capabilities.

| Capability | What it means |
|---|---|
| `catalog.lists` | the manifest can expose named browse `lists`, including lists fetched from `browse.lists_url` |
| `catalog.notices` | a browse list can show a confirmation notice (title, message, Confirm / Cancel labels) before it opens |
| `catalog.bundle_install` | a catalog entry can install a multi-file bundle (base URL + file list) |
| `catalog.version_tracking` | catalog versions are compared with installed manifests (Installed / Update) |
| `catalog.compatibility` | JSON catalog entries are filtered by firmware family, minimum plugin API and required capabilities (`browse.compatibility`) |
| `catalog.item_details` | JSON catalog items open a Details screen (description, author, repository, version, optional `details_url` text/HTML page) before Fetch / Install / Update |
| `catalog.encoded_item_fields` | download and sidecar templates accept `{id_enc}`, `{title_enc}`, `{author_enc}`, `{url_enc}` |
| `catalog.list_index_mapping` | `browse.lists_url` is fetched with `lists_headers` and read through `lists_items` / `lists_fields`; a list's `id` fills `{list_id}` / `{list_id_enc}` |
| `catalog.list_requirements` | manifest and fetched lists may carry a `requires` array of capabilities; a list needing one we lack is hidden |
| `catalog.server_paths` | where a response supplies a URL (the `download.url_path` answer, a fetched list `url`, an item `details_url`), a server path (`/path`) is joined to the origin of the request that returned it |

### Schema

Every string field is a template. Available substitutions:

| Variable | Meaning | Available in |
|---|---|---|
| `{token}` | contents of the token file at `token.path` | everywhere |
| `{cfg.KEY}` | value of `KEY` in the flat JSON config file at `config.file` | everywhere |
| `{firmware_family}` | `crosspoint` (the plugin contract family this firmware keeps) | everywhere |
| `{firmware_version}` | the firmware's `CROSSPOINT_VERSION` | everywhere |
| `{plugin_api}` | numeric plugin API level (`1`) | everywhere |
| `{plugin_capabilities}` | comma-separated advertised capabilities (see below) | everywhere |
| `{page}` | current 1-based page | browse |
| `{offset}` | rows before the current page: `(page - 1) * page_size` | browse |
| `{list_id}` `{list_id_enc}` | the open list's `id` as received / percent-encoded; empty outside a list | browse, download, sidecar |
| `{limit}` | `page_size` when the request uses `{page}` and not `{offset}`; otherwise `page_size + 1` (the extra row shows that another page exists) | browse |
| `{id}` `{title}` `{author}` `{url}` | fields of the selected item, as received | download, sidecar, item details |
| `{id_enc}` `{title_enc}` `{author_enc}` `{url_enc}` | the same fields percent-encoded (everything except letters, digits, `-_.~`), for query values and path segments | download, sidecar, item details |
| `{md5}` | MD5 hex of the destination file path | sidecar |
| `{dest}` | sanitized on-SD path of the downloaded file | sidecar |
| `{device_code}` | from the auth request response | auth poll |

Substitution inserts values as they are. Before a request is sent, spaces,
control characters and non-ASCII (UTF-8) bytes in its URL are percent-encoded,
so a raw `{title}` in a URL path still forms a valid request. Every other
printable ASCII character is kept (`&`, `?`, `#`, `/`, `+`, `=`, `%`), so use
the `_enc` forms for values inside a query string or a single path segment.

Two browse formats:
- **`"json"`** (default) — a paged JSON list, using `items`/`fields` below.
- **`"xml"`** — walks a repeating XML element with optional folder navigation
  (Confirm opens a folder, Back goes up). No paging; the JSON `items`/
  `page_size` are ignored. The firmware knows no specific protocol: a WebDAV
  `PROPFIND` multistatus, an OPDS/Atom feed, or any XML list is all manifest
  config. XML field selectors are `"elem"` (child element text), `"elem@attr"`
  (child element attribute), or `"@attr"` (attribute on the item element).

```jsonc
{
  "title": "Service Name",                  // menu label; defaults to folder name

  // Relative paths are in the plugin's own folder; absolute paths work too.
  // The credential stores (wifi/opds/koreader/settings .json) are refused.
  "token": {                                // omit for token-less catalogs
    "file": "token.json",                   // written by auth (either side)
    "path": "token"                         // dotted JSON path inside the file
  },

  "config": {                               // optional: flat JSON of {cfg.KEY} values,
    "file": "config.json"                   // e.g. a user-entered server URL + credentials
  },

  "browse": {                               // required
    "format": "json",                       // or "xml"
    "url": "https://.../search",
    "method": "POST",                       // default GET
    "headers": { "Authorization": "Bearer {token}" },
    "body": "{\"page\":{page},\"per_page\":{limit}}",

    // --- json format ---
    "items": "",                            // dotted path to the item array; "" = response root
    "fields": {                             // json: dotted paths (numeric = array index)
      "title": "title",                     // required (items without one are dropped)
      "author": "authors.0.name",
      "id": "id",
      "url": "download_url",                // when the item carries a direct file URL
      "version": "version"                  // catalog-of-plugins only: badges each row
                                            // Installed / Update by comparing this to the
                                            // installed plugin's manifest.json (found by id).
                                            // Both are MAJOR.MINOR.PATCH ("1.2.0"); only a
                                            // newer catalog version shows Update.
      "description": "description",         // optional: paged text on the Details screen
      "detail_author": "authors.0.name",    // optional Details author; defaults to author
      "repository": "repository",           // optional: line under the author on Details
      "details_url": "details.url",         // optional: fetched only when Details opens
      "details_format": "details.format"    // optional: "html" or plain text (default)
    },
    "compatibility": {                      // optional (json): entry fields checked against
      "firmware_family": "compatibility.firmware_family",   // comma-separated allow-list
      "plugin_api_min": "compatibility.plugin_api_min",     // minimum plugin API
      "requires": "compatibility.requires"                  // capabilities (string or array)
    },
    "page_size": 8,                         // 1..16; see Pagination below

    "lists": [                              // optional named sub-catalogs (json only):
      { "title": "All Books" },             // a picker screen precedes browsing;
      { "title": "Favorites",               // each entry may override url and/or
        "body": "{\"page\":{page},\"per_page\":{limit},\"list\":\"favorites\"}",
        "requires": ["catalog.compatibility"],  // optional: hidden unless all are advertised
        "notice": {                         // optional confirmation before opening
          "title": "Community content",     // defaults to the list title
          "message": "Review this notice before continuing.",
          "confirm": "Continue",            // defaults to translated Confirm
          "cancel": "Go Back"               // defaults to translated Cancel
        } }
    ],                                      // omitted keys fall back to browse's;
                                            // an "id" fills {list_id} / {list_id_enc}

    "lists_url": "https://.../folders",     // optional: more lists, shown after "lists"
    "lists_headers": { "Authorization": "Bearer {token}" },  // sent only to lists_url
    "lists_items": "folders",               // dotted path to the array; default "lists"
    "lists_fields": { "title": "title", "id": "folder_id" },  // defaults: title, url, id, requires

    // --- xml format (ignore the json fields above; fields become selectors) ---
    "item": "response",                     // local-name of the repeating element
    "container_element": "collection",      // presence marks a navigable folder (omit = flat)
    "skip_self": true,                      // drop the entry equal to the request URL
    "resolve_urls": true,                   // resolve url field against the request origin
    "extensions": [".epub", ".pdf"]         // allowed file extensions (omit = all)
  },

  "download": {
    "url": "https://.../books/{id}/download",
    "method": "POST",
    "headers": { "Authorization": "Bearer {token}" },  // file GET too, unless url_path
                                            // returns a full URL
    "body": "{}",
    "url_path": "url",                      // response field with the file URL or a
                                            // server path ("/files/1");
                                            // omit to treat "url" itself as the file URL
    "username": "{cfg.user}",              // optional HTTP Basic creds for the file GET
    "password": "{cfg.pass}",             // omit for token/header auth
    "dest_dir": "/ServiceName",             // created if missing; falls back to SD root
    "filename": "{title}.epub",             // rendered filename is sanitized to 100 bytes;
                                            // a conventional extension is preserved
    "sidecar": {                            // optional per-book metadata file
      "path": "{dest}.meta.json",             // the book metadata sidecar convention (see plugin-events.md)
      "body": "{\"book_id\":{id}}"
    }
  },

  "auth": {                                 // optional on-device sign-in
    "type": "device_code",                  // or "password"

    // device_code (interactive: shows a code + QR, polls until authorized):
    "request": { "url": "...", "method": "POST", "headers": { ... }, "body": "..." },
    "poll":    { "url": "...", "method": "POST", "headers": { ... },
                 "body": "{...\"device_code\":\"{device_code}\"}" },
    // device_code response field paths, with their defaults:
    "code_path": "user_code", "verify_url_path": "verification_uri",
    "device_code_path": "device_code", "interval_path": "interval",
    "expires_path": "expires_in", "token_path": "access_token",
    "error_path": "error"

    // password (silent: mints a token from stored config credentials — for
    // OAuth2 password grants and login-returns-token APIs). Only "request" and
    // "token_path" apply; no user interaction:
    //   "type": "password",
    //   "request": { "url": "{cfg.url}/oauth/v2/token", "method": "POST",
    //                "headers": {...}, "body": "...{cfg.username}...{cfg.password}..." },
    //   "token_path": "access_token"
  }
}
```

### Behavior

- **Sign-in (`device_code`)**: the not-signed-in screen offers Sign in on the
  device: it shows the verification URL (text + QR) and user code, then polls
  until authorized, honoring `authorization_pending`, `slow_down`,
  `expired_token`, and `access_denied`. The token is written to `token.file`
  in the shape `token.path` expects, so the web plugin and the device share
  one sign-in.
- **Sign-in (`password`)**: no on-device UI — the device silently POSTs the
  stored config credentials to the token endpoint before browsing, and re-mints
  a fresh token automatically after any 401 (these tokens are short-lived).
  Credentials come from a browser `plugin.js` writing the `config.file`. Set it
  up once on the web page; the reader is then standalone.
- Without an `auth` block the not-signed-in screen directs the user to the web
  plugin.
- **Stale tokens**: a 401/403 from browse returns to the sign-in screen rather
  than an error.
- **Missing config**: when the manifest names a `config.file` that does not
  exist (or does not parse), the device shows "Plugin not set up" and points at
  the web Plugins tab. A key the file leaves out substitutes as an empty value
  (`?w=&h=` rather than a literal `{cfg.width}`), matching the "omitted keys use
  the server's default" convention the plugin READMEs document.
- **Server certificates**: the X4 Pro / X4C accept RSA keys up to 4096 bits
  (`CROSSPOINT_WOLFSSL_SP_4096`); a host with a larger key fails the TLS
  handshake with `MP_INIT_E` in the serial log.
- **Pagination** (json): the variables a request uses choose how it pages
  (`{offset}` takes precedence over `{page}`), the same rules as upstream
  CrossPoint 2.0 (`src/activities/plugins/PluginCatalogPaging.h`):
  - `{page}` with `{limit}` (`?page={page}&per_page={limit}`): `{limit}` is
    `page_size`, so pages line up with the server's. A full page shows
    "Next page"; when the last page is exactly full, the next one is empty.
  - `{offset}` (`?offset={offset}&limit={limit}`): `{limit}` is
    `page_size + 1`, and the extra row shows that another page exists.
  - `{page}` without `{limit}` (one file per page, e.g. `all-{page}.json`):
    "Next page" appears when the response has more than `page_size` rows, so
    add one extra row at the end of every file except the last.
  - Neither `{page}` nor `{offset}`: the response is one static list, and the
    firmware pages through it locally.

  Rows without a title are not shown. When the request uses `{page}` or
  `{offset}` they still count toward the server's page, so a page can show
  fewer rows and still offer "Next page". Navigation matches the OPDS
  browser: a "Previous page" row precedes the items past page 1 and a "Next
  page" row follows them while more pages exist — tappable and
  button-reachable like any row.
- **Lists** (json): with `browse.lists`, opening the catalog shows a picker
  of the named entries first (a single list skips the picker and opens
  directly, after its notice; Back then leaves the catalog); each entry
  browses with its own url/body overrides (server-side categories, shelves,
  sort orders). A list with `notice.message` shows a paged confirmation
  first: Confirm continues into the list, Back returns to the picker. Back
  from the book list returns to the picker.
- **Dynamic lists** (json): `browse.lists_url` is fetched with a GET once per
  catalog visit, carrying `lists_headers` and no other headers. Entries come
  from the array at `lists_items`; each entry's title, url and id from the
  dotted paths in `lists_fields`, plus optional `body` and `notice` read by the
  same names as in `lists`. An entry needs a title and a url or an id (an
  id-only entry browses `browse.url` with `{list_id}`). At most 64 fetched
  lists are kept; an answer with no usable entries, or one larger than 16KB,
  counts as a failed fetch. The fetched lists follow the manifest's own; if the
  fetch fails the manifest's lists are still shown (not retried until the
  catalog is reopened), and without any the catalog shows an error that Retry
  fetches again. A 401/403 is handled as for browsing (password grant re-mint,
  else the sign-in screen).
- **List requirements**: an optional `requires` array on a manifest list or a
  fetched list (path `lists_fields.requires`) names firmware capabilities; the
  list is shown only when every one is advertised. A malformed array or more
  than 32 names hides the list.
- **Compatibility** (json): with `browse.compatibility`, an entry whose
  `firmware_family` list lacks `crosspoint`, whose `plugin_api_min` is above
  `1`, or whose `requires` names a capability not advertised is silently
  omitted. Entries without these fields stay visible. On a static feed the
  page window counts only compatible entries.
- **Item details** (json): when a catalog maps `description`, `repository`
  or `details_url` (or tracks versions, which implies the conventional
  `description` / `author` / `repository` / `details.url` / `details.format`
  keys), Confirm on an item opens a Details screen first: author, repository,
  version and install state, then the description. Confirm there fetches /
  installs / updates, Back returns to the list. The fields are read for the
  selected row only, from the browse response kept on SD while the page is
  shown, so long descriptions are never held for a whole page (description
  capped at 8 KB). A `details_url` is fetched only then (GET, no headers,
  24 KB cap); `details_format: "html"` is shown as readable text, an empty
  format or `text` as is. A failed or unsupported page falls back to the
  description and never blocks the download.
- **Server paths** (json): a URL a response supplies may be a server path
  such as `/api/files/12/download` — the `download.url_path` answer, a
  fetched list's `url`, or an item's `details_url`. The firmware joins it to the origin (scheme, host and
  port) of the request that returned it. A `url_path` answer that is a server
  path is on the API's own host, so the file GET sends the download `headers`
  (and a 401/403 there gets the password-grant retry); a full URL, even on the
  same host, gets no headers, because it may be pre-signed. Other relative
  forms (`files/12`, `//host/x`) are used as they are, and item `url` fields
  are not changed.
- **XML navigation**: Confirm on a folder (an item carrying `container_element`)
  descends into it, Back climbs out (Back at the root leaves the screen).
  With `resolve_urls`, server-relative URLs resolve against the browse URL's
  origin. A `config.file` lets values like a server URL and credentials be
  user-entered (via a browser `plugin.js`) rather than baked into the manifest.
- **After download**: the book's layout cache is invalidated and the optional
  sidecar is written. Use the `{dest}.meta.json` convention so the fields ride
  KOSync progress uploads and event handlers (see `plugin-events.md`).

### Heap budget (ESP32-C3, ~380KB total)

- Manifest ≤ 8KB, token file ≤ 2KB, small API responses (auth, download-url
  hop) ≤ 48KB (hard caps).
- Browse responses (json and xml) stream to an SD temp file and are parsed
  from it, so the raw body never occupies DRAM — services that inline heavy
  per-item metadata (a page of BookFusion JSON runs 60+ KB) work unmodified.
  A 1MB cap bounds a misbehaving server.
- Catalog responses are parsed with a dynamically built ArduinoJson filter
  containing only the declared field paths, so a page parses into a few KB
  no matter the body size.
- One TLS session is kept alive across browse requests: repeated handshakes
  permanently fragment the heap.
- The book download streams to SD via `HttpDownloader`; file size is
  unconstrained.
- An XML list is parsed from the temp file in 2KB chunks; the engine stops
  after 200 entries per folder/feed. Split very large libraries into
  subfolders.

### Testing a new manifest

1. Copy the plugin folder to `/.crosspoint/plugins/<name>/` on the SD card.
2. Settings → System → Plugins → your title. With no token and no `auth`
   block you should see the not-signed-in screen; with `auth`, the code/QR
   screen.
3. Watch serial (`[PCAT]` tag) for request/parse failures — the log includes
   HTTP status and truncation flags.
4. Token-less services (public JSON APIs) work by omitting the `token` block
   entirely; that is the quickest way to validate `items`/`fields` paths.

## Surface 4: firmware events and the book metadata sidecar

The firmware can record whitelisted moments (a reading session ended, the
device is entering sleep, a catalog download finished) into a per-plugin queue
and later replay them through HTTP request templates declared in
`device.json` — including bounded WiFi bring-up at sleep entry for handlers
that opt in. A companion convention, the `<book path>.meta.json` sidecar,
lets plugins attach service fields (e.g. a service book id) to a book; those
fields ride along with KOSync progress uploads and are available to event
handlers as `{meta.*}` variables. See `plugin-events.md` for the whole
surface: the event whitelist, handler schema, delivery semantics, and limits.

## Ideas to build

The two planes decide *how* you build something, so pick by what the idea needs:

- **On-device (`device.json`)** — standalone on the reader, but only the
  declarative vocabulary above: an authenticated JSON or XML/OPDS-style catalog,
  browse, download to SD, optional device-code or password sign-in and per-book
  sidecar. No arbitrary logic.
- **Browser (`plugin.js`)** — anything at all (any protocol, auth, format), but
  it runs in a connected browser/app. Use it when the idea needs request
  signing, HTML→EPUB conversion, multi-file writes, or a real UI. The job queue
  (`/api/plugin-jobs` + `/plugins-run`) lets an external app trigger it.

Rule of thumb: if the service is "log in, list an authenticated JSON/XML
catalog, download files," it's on-device. If it needs computed request bodies
(OAuth 1.0a signing, canonicalized signatures) or format conversion, it's
browser-side.

### On-device (`device.json`) ideas

- **Self-hosted library presets** — Kavita, Komga, Audiobookshelf, Calibre-Web,
  COPS. Most expose OPDS (native OPDS browsing already covers those — a plugin
  adds value mainly for their JSON APIs or token auth). Build: `browse.format`
  `json` against the REST API, or `xml` against the OPDS feed; `download.url`
  from the entry; bearer/Basic/`{cfg.*}` auth. See `bookfusion` (JSON + token)
  and `webdav` (XML + config creds).
- **Dictionary store** — browse a JSON index of dictionary files, download to
  the SD dictionary dir. No native equivalent. Build: token-less `json` catalog,
  `download.dest_dir` = the dictionary folder, no sidecar. ~15 lines.
- **Theme store / font store** — same shape as the dictionary store, pointed at
  the theme or font directory. Asset catalogs OPDS can't express.
- **More read-later that exports EPUB** — anything with a token API and an
  EPUB export URL fits like `wallabag`: `auth.type` `password` (or a stored
  bearer), `browse` the entries JSON, `download` the export endpoint with a
  Bearer header.
- **A curated free-books catalog** — Standard Ebooks / Gutenberg via their OPDS
  feed (`xml` format, token-less). Overlaps native OPDS, but ships as a
  zero-config preset.

### Browser (`plugin.js`) ideas

- **Plugin store** — install other plugins from a hosted catalog. Build: fetch
  a catalog with `api.relay`, `/mkdir` the target folder, `api.writeFile` each
  file. See `plugin-store/`.
- **AO3 / fanfiction downloader** — the site serves per-work EPUB downloads.
  Build: take a work URL, `api.fetchToSd` (or `api.relay` + `api.writeFile`) the
  EPUB to SD. Browsing/search is HTML, so parse it in the browser.
- **RSS → EPUB digest** — build a "morning edition." Fetch feeds with
  `api.relay`, assemble an EPUB in the browser with the JSZip the File Manager
  page already loads, then `api.writeFile` it to SD.
- **Read-any-article** — paste a URL, run it through a readability service via
  `api.relay`, wrap the result as an EPUB, write to SD.
- **Metadata / cover editor** — a File Manager plugin: open an EPUB with JSZip,
  edit the OPF (title/author/cover), rewrite it in place. See
  `organize-by-author/` for the File-Manager-endpoints pattern.
- **Library tidy tools** — dedupe, bulk rename, move-by-metadata. Same
  same-origin endpoints (`/api/files`, `/mkdir`, `/move`, `/delete`), no device
  API needed.
- **Signed-API services** (Instapaper OAuth 1.0a, etc.) — must be browser-side:
  do the per-request signing in JS with `crypto.subtle`, relay the calls, and
  convert the article HTML to EPUB before writing to SD.

### Building blocks reference

| Need | Use |
|---|---|
| Outbound HTTP(S), any method (CORS-free) | `api.relay(method, url, headers, body)` |
| Download a URL straight to SD | `api.fetchToSd(url, dest, headers, {method, body})` (options optional; `body` is a string up to 4 KB; with a method other than GET, or a non-empty body, the request is sent once with no resume, and on failure the thrown error carries the `/api/fetch` result in `err.result`) |
| Write a small file to SD | `api.writeFile(path, base64)` |
| The plugin's own folder (keep files beside yourself) | `api.dir` (`<root>/<name>`, from `/api/plugins`) |
| Crypto (hash, HMAC, AES, RSA, PKCS#12) | `api.crypto(op, fields)` |
| Create / delete / move SD files | same-origin `/mkdir`, `/delete`, `/move` |
| On-device catalog/browse/download | `device.json` (this document) |
| External app triggers an action | `api.registerAction(name, fn)` + `/api/plugin-jobs` |

## Not carried: protected content

Upstream's branch pairs the plugin host with a decrypt-on-read path for
content-protected EPUBs. This fork deliberately carries none of that (no
`ContentProtection` library, no rights files; `/api/crypto` is implemented
directly on wolfCrypt in `src/network/CrossPointWebServerCrypto.cpp` and does
not link that library), and it must not be reintroduced. A plugin can
download whatever its service serves, but the reader only opens plain books.
