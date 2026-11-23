#pragma once

#include <cstddef>

/**
 * Append-only diagnostic log for failed downloads, written to the SD card so
 * a user can send it in with a bug report instead of a serial capture.
 *
 * Every call opens, appends and closes the file, so a crash or power loss in
 * the middle of a transfer loses nothing already written. The file is rotated
 * to `<path>.old` once it passes kMaxBytes, so it can never fill the card.
 *
 * Nothing here is on a hot path: it is only called around a transfer's start
 * and after a failure. Formatting goes through a 192-byte stack buffer; longer
 * lines are truncated.
 */
namespace DownloadFailureLog {

constexpr const char* kPath = "/.crosspoint/download_failure.log";
constexpr size_t kMaxBytes = 96 * 1024;

// One timestamped line, printf-style.
void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Banner with wall clock, uptime and firmware version. Also performs rotation.
void section(const char* title);

// Heap, task stack and Wi-Fi snapshot — the two things every download failure
// so far has come down to.
void environment(const char* label);

// URL with the query-string VALUES blanked (pre-signed CDN URLs carry a
// signature that is a credential until it expires). Keeps scheme, host, path
// and the parameter names.
void url(const char* label, const char* fullUrl);

// Network reachability of one host, to tell a block from a reader bug: what
// DNS answers (flagged when it is 0.0.0.0, loopback or a private address, the
// usual signs of a DNS filter) and whether a TCP connection to port 443 opens,
// with its time. Costs up to ~4 s per host; only for a transfer that already
// failed at the transport level.
void hostProbe(const char* host);

// hostProbe() for the hosts every GitHub-backed download goes through (fonts,
// dictionaries, firmware updates), so one report shows which of them the
// network refuses.
void githubProbe();

}  // namespace DownloadFailureLog
