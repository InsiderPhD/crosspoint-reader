#pragma once

#include <cstdint>

class GfxRenderer;

// Reading stats over the CrossPoint Sync stats extension, driven by Sync
// Books when What to Sync > Reading Stats is on. Built from the resident
// READING_STATS store; nothing is re-read from the card.
//
// Only ever runs from Sync Books (outside the reader: no book open, Bluetooth
// off), only to a CrossPoint/Other server. Heap: one 4KB payload buffer for
// the whole step (too big for the stack; freed at return) plus the TLS
// request, which runs inside a TlsFramebufferBorrow like every C3 request.
namespace StatsSync {

enum class Result : uint8_t {
  Off,          // Reading Stats sync turned off, or a strict KOSync server
  Ok,           // everything sent
  Unsupported,  // the server has no stats routes (KOReader-only server)
  Failed,       // network, auth or server error
  LowMemory,    // refused before a request: heap too low for TLS
};

// Overall snapshot (with up to 20 days of daily reading time), then every
// book's snapshot, several per request. `booksSent` counts accepted books.
Result upload(GfxRenderer& renderer, unsigned& booksSent);

// "Done" / "Not supported" / "Off" / an error, for result screens.
const char* label(Result result);

}  // namespace StatsSync
