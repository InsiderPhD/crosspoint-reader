#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class GfxRenderer;

// Reading-session journal for CrossPoint Sync (PUT /api/v1/stats/sessions,
// CrossInk c29cba9c3): one record per counted session with its wall-clock
// start and end, so a server or companion app (BookOrbit) can show when you
// read, not just how much. Sessions never add to the server's totals; the
// stats snapshots do that.
//
// Records queue in an append-only outbox on the card
// (/.crosspoint/kosync_sessions.jsonl, capped at OUTBOX_LIMIT bytes) and go
// up with the next Sync Books; nothing is held in RAM between syncs. Only
// recorded while Sync Reading Stats is on, the server is CrossPoint/Other and
// the clock was valid for the whole session.
namespace SessionJournal {

constexpr size_t OUTBOX_LIMIT = 48 * 1024;  // ~250 sessions

// Called by ReadingStatsStore::endSession for a counted session.
void record(const std::string& path, uint32_t startedAt, uint32_t activeMs);

enum class Result : uint8_t { Ok, Unsupported, Failed };

// Send the outbox, up to 20 sessions per request, removing what the server
// confirmed by id. `buffer` is the stats step's payload buffer.
Result upload(GfxRenderer& renderer, char* buffer, size_t capacity, const char* deviceId, unsigned& sent);

}  // namespace SessionJournal
