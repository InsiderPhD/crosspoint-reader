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

// Network reachability of one host, to tell a block from a reader bug, in one
// report: what this network's DNS answers (flagged when it is 0.0.0.0,
// loopback or a private address, the usual signs of a DNS filter), what
// Cloudflare (1.1.1.1) and Google (8.8.8.8) answer when asked directly, whether
// TCP port 443 opens at each address, and an HTTPS HEAD / (status, Server
// header, redirect, TLS protocol, or the TLS error and alert). Seconds per host;
// only after a transfer already failed.
void hostProbe(const char* host);

// hostProbe() for the hosts every GitHub-backed download goes through (fonts,
// dictionaries, firmware updates) plus `failingHost` when it is another one,
// then a one-line verdict. Once per boot: a retried download points at the
// report already written.
void githubProbe(const char* failingHost = nullptr);

// The first bytes of a downloaded file, printable or hex, for a transfer that
// "succeeded" but whose content was refused (a filter's block page saved as
// the font list, firmware or dictionary catalog).
void fileHead(const char* label, const char* path);

}  // namespace DownloadFailureLog
