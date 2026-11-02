# Webserver Endpoints

This document describes all HTTP and WebSocket endpoints available on the CrossPoint Reader webserver.

- [Webserver Endpoints](#webserver-endpoints)
  - [Overview](#overview)
  - [HTTP Endpoints](#http-endpoints)
    - [GET `/` - Home Page](#get----home-page)
    - [GET `/files` - File Browser Page](#get-files---file-browser-page)
    - [GET `/api/status` - Device Status](#get-apistatus---device-status)
    - [GET `/api/files` - List Files](#get-apifiles---list-files)
    - [GET `/download` - Download a File](#get-download---download-a-file)
    - [POST `/upload` - Upload File](#post-upload---upload-file)
    - [POST `/mkdir` - Create Folder](#post-mkdir---create-folder)
    - [POST `/delete` - Delete File or Folder](#post-delete---delete-file-or-folder)
    - [POST `/rename` - Rename a File](#post-rename---rename-a-file)
    - [POST `/move` - Move a File](#post-move---move-a-file)
    - [GET `/settings` - Settings Page](#get-settings---settings-page)
    - [GET `/api/settings` - Read Settings](#get-apisettings---read-settings)
    - [POST `/api/settings` - Write Settings](#post-apisettings---write-settings)
    - [POST `/api/clear-library-data` - Clear Library Data](#post-apiclear-library-data---clear-library-data)
    - [GET `/fonts` - Font Manager Page](#get-fonts---font-manager-page)
    - [GET `/api/fonts` - List Installed Fonts](#get-apifonts---list-installed-fonts)
    - [POST `/api/fonts/upload` - Upload a Font](#post-apifontsupload---upload-a-font)
    - [POST `/api/fonts/delete` - Delete a Font Family](#post-apifontsdelete---delete-a-font-family)
    - [SD-card plugin endpoints (X4 Pro / X4C)](#sd-card-plugin-endpoints-x4-pro--x4c)
  - [WebSocket Endpoint](#websocket-endpoint)
    - [Port 81 - Fast Binary Upload](#port-81---fast-binary-upload)
  - [Network Modes](#network-modes)
    - [Station Mode (STA)](#station-mode-sta)
    - [Access Point Mode (AP)](#access-point-mode-ap)
  - [Notes](#notes)


## Overview

The CrossPoint Reader exposes a webserver for file management and device monitoring:

- **HTTP Server**: Port 80 — file management, a settings editor, and SD-card font management
- **WebSocket Server**: Port 81 (for fast binary uploads)

---

## HTTP Endpoints

### GET `/` - Home Page

Serves the home page HTML interface.

**Request:**
```bash
curl http://crosspoint.local/
```

**Response:** HTML page (200 OK)

---

### GET `/files` - File Browser Page

Serves the file browser HTML interface.

**Request:**
```bash
curl http://crosspoint.local/files
```

**Response:** HTML page (200 OK)

---

### GET `/api/status` - Device Status

Returns JSON with device status information.

**Request:**
```bash
curl http://crosspoint.local/api/status
```

**Response (200 OK):**
```json
{
  "version": "1.0.0",
  "firmwareFamily": "crosspoint",
  "pluginApi": 1,
  "pluginCapabilities": "catalog.lists,catalog.notices,...,events.sleep.enter",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600
}
```

| Field      | Type   | Description                                               |
| ---------- | ------ | --------------------------------------------------------- |
| `version`  | string | CrossPoint firmware version                               |
| `firmwareFamily` | string | Plugin contract family: `"crosspoint"` (shared with upstream CrossPoint) |
| `pluginApi` | number | Plugin API level: `1` on X4 Pro / X4C, `0` on boards without plugin support |
| `pluginCapabilities` | string | Comma-separated plugin capabilities this build implements (see `docs/sd-plugins.md`); empty without plugin support |
| `ip`       | string | Device IP address                                         |
| `mode`     | string | `"STA"` (connected to WiFi) or `"AP"` (access point mode) |
| `rssi`     | number | WiFi signal strength in dBm (0 in AP mode)                |
| `freeHeap` | number | Free heap memory in bytes                                 |
| `uptime`   | number | Seconds since device boot                                 |
| `device`   | string | Board name (`X3`, `X4`, or the S3 board's name)           |

No `deviceId` is returned (upstream derives a per-plugin id from a device
secret this firmware does not have), and `?plugin=` is ignored.

---

### GET `/api/files` - List Files

Returns a JSON array of files and folders in the specified directory.

**Request:**
```bash
# List root directory
curl http://crosspoint.local/api/files

# List specific directory
curl "http://crosspoint.local/api/files?path=/Books"
```

**Query Parameters:**

| Parameter | Required | Default | Description            |
| --------- | -------- | ------- | ---------------------- |
| `path`    | No       | `/`     | Directory path to list |

**Response (200 OK):**
```json
[
  {"name": "MyBook.epub", "size": 1234567, "isDirectory": false, "isEpub": true},
  {"name": "Notes", "size": 0, "isDirectory": true, "isEpub": false},
  {"name": "document.pdf", "size": 54321, "isDirectory": false, "isEpub": false}
]
```

| Field         | Type    | Description                              |
| ------------- | ------- | ---------------------------------------- |
| `name`        | string  | File or folder name                      |
| `size`        | number  | Size in bytes (0 for directories)        |
| `isDirectory` | boolean | `true` if the item is a folder           |
| `isEpub`      | boolean | `true` if the file has `.epub` extension |

**Notes:**
- Hidden files (starting with `.`) are automatically filtered out
- System folders (`System Volume Information`, `XTCache`) are hidden

---

### GET `/download` - Download a File

Streams a file from the SD card to the client.

**Request:**
```bash
curl -O -J "http://crosspoint.local/download?path=/Books/mybook.epub"
```

**Query Parameters:**

| Parameter | Required | Description                  |
| --------- | -------- | ---------------------------- |
| `path`    | Yes      | Path to the file to download |

**Response:** the file body with `Content-Disposition: attachment` (200 OK)

**Error Responses:**

| Status | Body                         | Cause                         |
| ------ | ---------------------------- | ----------------------------- |
| 400    | `Missing path`               | `path` parameter not provided |
| 400    | `Invalid path`               | Empty path, or `/`            |
| 403    | `Cannot access system files` | Hidden file (starts with `.`) |
| 404    | `Item not found`             | Path does not exist           |

---

### POST `/upload` - Upload File

Uploads a file to the SD card via multipart form data.

**Request:**
```bash
# Upload to root directory
curl -X POST -F "file=@mybook.epub" http://crosspoint.local/upload

# Upload to specific directory
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

**Query Parameters:**

| Parameter | Required | Default | Description                     |
| --------- | -------- | ------- | ------------------------------- |
| `path`    | No       | `/`     | Target directory for the upload |

**Response (200 OK):**
```
File uploaded successfully: mybook.epub
```

**Error Responses:**

| Status | Body                                            | Cause                       |
| ------ | ----------------------------------------------- | --------------------------- |
| 400    | `Failed to create file on SD card`              | Cannot create file          |
| 400    | `Failed to write to SD card - disk may be full` | Write error during upload   |
| 400    | `Failed to write final data to SD card`         | Error flushing final buffer |
| 400    | `Upload aborted`                                | Client aborted the upload   |
| 400    | `Unknown error during upload`                   | Unspecified error           |

**Notes:**
- An existing file with the same name is replaced. The upload is written to `<name>.part` and replaces the file only once it is complete, so a failed upload keeps the old file (the card needs room for both while it runs). A folder with the name fails with `400`. The `overwrite` query parameter is accepted for upstream compatibility and changes nothing.
- Uses a 4KB buffer for efficient SD card writes

---

### POST `/mkdir` - Create Folder

Creates a new folder on the SD card.

**Request:**
```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

**Form Parameters:**

| Parameter | Required | Default | Description                  |
| --------- | -------- | ------- | ---------------------------- |
| `name`    | Yes      | -       | Name of the folder to create |
| `path`    | No       | `/`     | Parent directory path        |

**Response (200 OK):**
```
Folder created: NewFolder
```

**Error Responses:**

| Status | Body                          | Cause                         |
| ------ | ----------------------------- | ----------------------------- |
| 400    | `Missing folder name`         | `name` parameter not provided |
| 400    | `Folder name cannot be empty` | Empty folder name             |
| 400    | `Folder already exists`       | Folder with same name exists  |
| 500    | `Failed to create folder`     | SD card error                 |

---

### POST `/delete` - Delete File or Folder

Deletes a file or folder from the SD card.

**Request:**
```bash
# Delete a file
curl -X POST -d "path=/Books/mybook.epub&type=file" http://crosspoint.local/delete

# Delete an empty folder
curl -X POST -d "path=/OldFolder&type=folder" http://crosspoint.local/delete
```

**Form Parameters:**

| Parameter | Required | Default | Description                      |
| --------- | -------- | ------- | -------------------------------- |
| `path`    | Yes      | -       | Path to the item to delete       |
| `type`    | No       | `file`  | Type of item: `file` or `folder` |

**Response (200 OK):**
```
Deleted successfully
```

**Error Responses:**

| Status | Body                                          | Cause                         |
| ------ | --------------------------------------------- | ----------------------------- |
| 400    | `Missing path`                                | `path` parameter not provided |
| 400    | `Cannot delete root directory`                | Attempted to delete `/`       |
| 400    | `Folder is not empty. Delete contents first.` | Non-empty folder              |
| 403    | `Cannot delete system files`                  | Hidden file (starts with `.`) |
| 403    | `Cannot delete protected items`               | Protected system folder       |
| 404    | `Item not found`                              | Path does not exist           |
| 500    | `Failed to delete item`                       | SD card error                 |

**Protected Items:**
- Files/folders starting with `.`
- `System Volume Information`
- `XTCache`

---

### POST `/rename` - Rename a File

Renames a file in place. **Files only** — directories cannot be renamed.

**Request:**
```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

**Form Parameters:**

| Parameter | Required | Description                            |
| --------- | -------- | -------------------------------------- |
| `path`    | Yes      | Path to the file                       |
| `name`    | Yes      | New file name (no `/` or `\`)          |

**Error Responses:**

| Status | Body                              | Cause                                |
| ------ | --------------------------------- | ------------------------------------ |
| 400    | `Missing path or new name`        | A parameter was omitted              |
| 400    | `New name cannot be empty`        | Name was blank after trimming        |
| 400    | `Invalid file name`               | Name contained a path separator      |
| 400    | `Only files can be renamed`       | Path pointed at a directory          |
| 403    | `Cannot rename protected item`    | Protected source name                |
| 403    | `Cannot rename to protected name` | Protected destination name           |
| 404    | `Item not found`                  | Path does not exist                  |
| 500    | `Failed to rename file`           | SD card error                        |

A rename to the file's existing name returns `200 Name unchanged` without touching the card.

---

### POST `/move` - Move a File

Moves a file into an existing directory. **Files only.**

**Request:**
```bash
curl -X POST -d "path=/mybook.epub&dest=/Books" http://crosspoint.local/move
```

**Form Parameters:**

| Parameter | Required | Description                          |
| --------- | -------- | ------------------------------------ |
| `path`    | Yes      | Path to the file to move             |
| `dest`    | Yes      | Destination directory (must exist)   |

**Error Responses:**

| Status | Body                              | Cause                          |
| ------ | --------------------------------- | ------------------------------ |
| 400    | `Missing path or destination`     | A parameter was omitted        |
| 400    | `Only files can be moved`         | Path pointed at a directory    |
| 403    | `Cannot move protected item`      | Protected source               |
| 403    | `Cannot move into protected folder` | Protected destination        |
| 404    | `Item not found` / `Destination not found` | Path does not exist   |

---

### GET `/settings` - Settings Page

Serves the web settings editor HTML interface.

**Response:** HTML page (200 OK)

---

### GET `/api/settings` - Read Settings

Returns every device setting that carries a JSON key, streamed as a JSON array. Each
entry describes the setting's key, type (toggle / enum / value), current value, and —
for enums — the available options. This is the same list the device UI is built from
(`src/SettingsList.h`), so it grows automatically as settings are added.

**Request:**
```bash
curl http://crosspoint.local/api/settings
```

**Response:** `application/json` (200 OK)

---

### POST `/api/settings` - Write Settings

Applies a partial settings update. Send a JSON object keyed by setting key; unknown
keys are ignored, and enum/value settings are range-checked before being applied.
The keys are KatiePoint's (the ones `GET /api/settings` lists), saved to
`/.crosspoint/katiepoint-settings.json`; the shared subset is then mirrored into
upstream CrossPoint's `/.crosspoint/settings.json` in its numbering. Neither file
can be read or written through the file endpoints (see `ProtectedPaths.h`).

**Request:**
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"darkMode":1,"screenMargin":20}' http://crosspoint.local/api/settings
```

**Response (200 OK):**
```
Applied 2 setting(s)
```

**Error Responses:**

| Status | Body                  | Cause                    |
| ------ | --------------------- | ------------------------ |
| 400    | `Missing JSON body`   | No request body          |
| 400    | `Invalid JSON: <err>` | Body failed to parse     |

---

### POST `/api/clear-library-data` - Clear Library Data

Deletes `recent.json` and every `epub_*` / `xtc_*` cache directory under `/.crosspoint/`.
Reading progress stored in those directories goes with them.

**Response (200 OK):**
```
<n> item(s) cleared
```

Returns 500 with a `<cleared> cleared, <failed> failed` body if any item could not be removed.

---

### GET `/api/opds` - List OPDS Servers

Saved OPDS catalogs (`/.crosspoint/opds.json`). Same shape as upstream CrossPoint 2.0. Passwords and header
values are never returned, only whether one is set.

```json
[
  {
    "index": 0,
    "name": "My Catalog",
    "url": "http://calibre.local:8080/opds",
    "username": "reader",
    "hasPassword": true,
    "headers": [{"name": "CF-Access-Client-Id", "hasValue": true}, {"name": "", "hasValue": false}]
  }
]
```

---

### POST `/api/opds` - Add or Update an OPDS Server

JSON body (`Content-Type: application/json`). Without `index` it adds a server (400 once 8 are saved); with
`index` it replaces that one. On an update an omitted `password`, or an omitted header `value`, keeps the
stored secret; a header with an empty `name` clears that slot.

```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"name":"My Catalog","url":"http://calibre.local:8080/opds","username":"reader","password":"secret","headers":[{"name":"X-Token","value":"abc"}]}' \
  http://crosspoint.local/api/opds
```

**Response:** `200 OK` with body `OK`; `400` for a missing/invalid body or index.

---

### POST `/api/opds/delete` - Delete an OPDS Server

Body `{"index": 0}`. POST rather than DELETE because the ESP32 web server takes no DELETE body.

---

### GET `/api/wifi` - List Saved WiFi Networks

Passwords are never returned.

```json
[{"index": 0, "ssid": "HomeWiFi", "hasPassword": true, "isLastConnected": true}]
```

---

### POST `/api/wifi` - Add or Update a WiFi Network

Body `{"ssid": "HomeWiFi", "password": "secret"}`, plus `"index"` to update that entry (renaming it moves it to
the end of the list). On an update an omitted `password` keeps the stored one; an empty one means an open network.
`ssid` is required.

---

### POST `/api/wifi/delete` - Delete a WiFi Network

Body `{"index": 0}`.

---

### GET `/fonts` - Font Manager Page

Serves the SD-card font management HTML interface. See [sd-card-fonts.md](sd-card-fonts.md).

**Response:** HTML page (200 OK)

---

### GET `/api/fonts` - List Installed Fonts

Lists the SD-card font families the device has registered, with their available sizes
and the files backing each one.

**Response (200 OK):**
```json
{
  "families": [
    { "name": "Literata", "sizes": [12, 14, 16], "files": [{ "name": "Literata-12.cpfont", "size": 40960 }] }
  ],
  "maxFamilies": 4,
  "vectorFonts": false
}
```

`vectorFonts` is `true` on boards that render TrueType/OpenType files (X4 Pro, X4C). Such
a family reports an empty `sizes` array: it has no fixed sizes.

---

### POST `/api/fonts/upload` - Upload a Font

Multipart upload of a `.cpfont` file, or of a `.ttf`/`.otf` file (up to 3 MB) on boards
where `vectorFonts` is `true`. The `family` form field names the family the file belongs
to.

**Response:** `{"ok":true}` (200 OK), or `{"error":"<reason>"}` (400). A `.ttf`/`.otf`
upload to an X3 or X4 is refused with a message saying the font needs an X4 Pro or X4C.

---

### POST `/api/fonts/delete` - Delete a Font Family

Deletes every file belonging to one SD-card font family.

**Request:**
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' http://crosspoint.local/api/fonts/delete
```

**Response:** `{"ok":true}` (200 OK), `{"error":"Invalid request"}` (400), or `{"error":"Delete failed"}` (500).

---

### GET `/sleep-themes` - Sleep Themes Page

Serves the sleep-theme compiler: it lays out an uploaded HTML/CSS theme in the browser,
bakes the static parts into a 1-bit background, and installs the result under
`/sleep-themes/<name>/` with `/mkdir` and `/upload`. It lists installed themes with
`/api/files`, switches between them with `/api/settings`, and deletes one with `/delete`
(clearing `sleepTheme` when it was the selected one). See [file-formats.md](file-formats.md#sleep-themes-sleep-themesname).

**Response:** HTML page (200 OK)

---

### GET `/api/sleep-theme/vars` - Sleep Theme Values

Everything a theme can show, with the user's current values (the page previews with them),
plus the limits the compiler checks against.

**Response (200 OK):**
```json
{
  "panelWidth": 800, "panelHeight": 480, "themesDir": "/sleep-themes", "formatVersion": 1,
  "limits": { "elements": 64, "fonts": 4, "strings": 4096 },
  "active": "departures",
  "vars": [
    { "name": "stats.total_hours", "kind": "number", "value": "61", "raw": 61 },
    { "name": "time.now", "kind": "timestamp", "value": "21:04", "raw": 1792530240 },
    { "name": "book.title", "kind": "text", "value": "The War of the Worlds" }
  ]
}
```

`kind` is `number`, `text`, `timestamp` or `image` (`book.cover`, `recent.N.cover`,
`sleep.image`). An empty `value` means unknown right now (no book open, clock not set).
Making a theme the sleep screen is a `POST /api/settings` with `{"sleepTheme": "<name>"}`
(`""` for the built-in screen).

---

### SD-card plugin endpoints (X4 Pro / X4C)

Built only on the boards with plugin support (`CROSSPOINT_SD_PLUGINS`). The
action endpoints answer `403 plugin system disabled` while **Enable Plugins**
is off. Full contract: [docs/sd-plugins.md](sd-plugins.md).

| Endpoint | Purpose |
| --- | --- |
| `GET /plugins` | Plugins page (all boards; a one-line note where unsupported) |
| `GET /plugin-host.js`, `GET /js/plugin-host.js` | the shared browser plugin host script (gzip) |
| `GET /api/plugins` | discovered browser plugins `[{name, dir, title, mount}]` |
| `GET /plugin?name=&file=` | a plugin's file; `ETag` (content MD5) + `Cache-Control: no-cache`, `304` on `If-None-Match` |
| `GET /plugins-run` | headless page that runs plugin jobs |
| `POST /api/relay` | outbound HTTP(S) on a plugin's behalf |
| `POST /api/fetch` | download a URL straight to SD |
| `POST /api/crypto` | wolfCrypt primitives |
| `POST /api/plugin-fs` | write a small file to SD |
| `POST /api/plugin-jobs`, `GET /api/plugin-jobs/claim`, `POST /api/plugin-jobs/complete`, `GET /api/plugin-jobs/status` | plugin job queue |

---

## WebSocket Endpoint

### Port 81 - Fast Binary Upload

A WebSocket endpoint for high-speed binary file uploads. More efficient than HTTP multipart for large files.

**Connection:**
```
ws://crosspoint.local:81/
```

**Protocol:**

1. **Client** sends TEXT message: `START:<filename>:<size>:<path>`
2. **Server** responds with TEXT: `READY`
3. **Client** sends BINARY messages with file data chunks
4. **Server** sends TEXT progress updates: `PROGRESS:<received>:<total>`
5. **Server** sends TEXT when complete: `DONE` or `ERROR:<message>`

**Example Session:**

```
Client -> "START:mybook.epub:1234567:/Books"
Server -> "READY"
Client -> [binary chunk 1]
Client -> [binary chunk 2]
Server -> "PROGRESS:65536:1234567"
Client -> [binary chunk 3]
...
Server -> "PROGRESS:1234567:1234567"
Server -> "DONE"
```

**Error Messages:**

| Message                           | Cause                              |
| --------------------------------- | ---------------------------------- |
| `ERROR:Failed to create file`     | Cannot create file on SD card      |
| `ERROR:Invalid START format`      | Malformed START message            |
| `ERROR:No upload in progress`     | Binary data received without START |
| `ERROR:Write failed - disk full?` | SD card write error                |

**Example with `websocat`:**
```bash
# Interactive session
websocat ws://crosspoint.local:81

# Then type:
START:mybook.epub:1234567:/Books
# Wait for READY, then send binary data
```

**Notes:**
- Progress updates are sent every 64KB or at completion
- Disconnection during upload will delete the incomplete file
- Existing files with the same name will be overwritten

---

## Network Modes

The device can operate in two network modes:

### Station Mode (STA)
- Device connects to an existing WiFi network
- IP address assigned by router/DHCP
- `mode` field in `/api/status` returns `"STA"`
- `rssi` field shows signal strength

### Access Point Mode (AP)
- Device creates its own WiFi hotspot
- Default IP is typically `192.168.4.1`
- `mode` field in `/api/status` returns `"AP"`
- `rssi` field returns `0`

---

## Notes

- These examples use `crosspoint.local`. If your network does not support mDNS or the address does not resolve, replace it with the specific **IP Address** displayed on your device screen (e.g., `http://192.168.1.102/`).
- All paths on the SD card start with `/`
- Trailing slashes are automatically stripped (except for root `/`)
- The webserver uses chunked transfer encoding for file listings
