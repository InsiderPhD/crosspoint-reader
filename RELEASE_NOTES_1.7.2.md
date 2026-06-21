# CrossPoint Reader 1.7.2

A maintenance re-release of 1.7.1 to make sure the over-the-air update is delivered reliably. **No functional changes from 1.7.1** — if you're already on 1.7.1 you're not missing anything. If you're coming from 1.7.0, everything below is what's new.

---

## 📥 Downloads no longer fail on large books

Downloading larger books — especially image-heavy BookFusion EPUBs — could fail outright with a `writeToStream error: -8` (out-of-memory) message. The downloader now streams data through a small buffer it manages itself, so transfers succeed even when the device is low on free memory. This applies to **all** downloads: BookFusion, OPDS/Calibre, firmware updates, and fonts.

## 🧩 No more corrupt covers from interrupted downloads

A download that got cut off partway through used to be saved as if it had completed, leaving a corrupt file (often a half-decoded cover image). Interrupted transfers are now detected and rejected, so you get a clean retry instead of a broken book.

## ⚠️ Heads-up before downloading huge books

Image-heavy books strain this device's limited memory. When you choose a BookFusion book that's **10 MB or larger**, you'll now see a quick confirmation showing the book's size and a warning that it may load slowly or be unstable — **Confirm** to download anyway, or **Back** to cancel. The size is known up front, so nothing is downloaded unless you say yes.

## 🖼️ BookFusion covers now show up reliably

BookFusion books now always use the cover image from BookFusion (already sized and normalised for the device) instead of trying to extract one from the EPUB — many BookFusion EPUBs carry no usable embedded cover, which is why some books showed a blank placeholder. Cover thumbnails are now produced as grayscale (the same dependable path the rest of the app uses), and the library falls back to the full BookFusion cover when a tile thumbnail is missing, so books you'd already downloaded get their covers back too.

## 📊 Your BookFusion reading position carries over

When you download a book you're partway through on another device, its reading percentage now shows on the book straight away in the library — taken directly from BookFusion (the authoritative source) instead of being recomputed and often showing 0%.

## 🔄 The library refreshes itself after you add books

Newly added books now appear in the **Library** immediately — whether they arrive via BookFusion, OPDS/Calibre, the web uploader, or WebDAV. Previously the library could keep showing a cached list and miss new books until something else forced a rescan. For rare edge cases, a **Recache Library** option (under **Developer Mode**) forces a full rescan on demand.

## 🌙 Cleaner sleep screen

The e-ink panel's charge pump is now switched off at the end of the sleep refresh, so the sleep image no longer picks up noise or ghosting as the device drops into deep sleep.

## ⚙️ Settings & stats tidy-ups

- Reading-stats settings (daily goal, minimum session length) now sit together with the rest of the Stats settings, on the device and on the web settings page.
- On the Reading Stats screen, the Confirm button hint now shows the next tab's name, matching the convention used elsewhere in the UI.

## 🐛 Fixes

- Fixed the Lyra theme's power-button hint box rendering over page content (it now has a solid background).

---

*Upgrading from 1.7.0 or 1.7.1: no action needed — cached books and settings are preserved. A BookFusion book downloaded earlier that still shows a blank cover will pick one up after a re-download (or via **Developer Mode → Recache Library**).*
