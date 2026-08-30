#pragma once

// Libby (OverDrive) loans, on device.
//
// SCOPE: this covers the things a reader does repeatedly -- list your loans,
// fulfil one, renew it, give it back. It deliberately does NOT cover linking a
// Libby account or authorising the reader for protected content; both are
// one-time, both need crypto that is not compiled into this firmware (RSA key
// generation, PKCS#1 encrypt, PKCS#12), and both stay in the web UI. See
// src/network/html/LibbyPage.html.
//
// What this reads, and who wrote it:
//   /.crosspoint/libby.json          the Libby chip (a JWT)   -- web UI
//   /.crosspoint/libby-session.json  the ADEPT signing keys   -- web UI
//   /.crosspoint/content.key         the reader's credential  -- web UI
//
// So a device that has never been set up in a browser reports NO_IDENTITY here
// rather than trying to fix itself.
//
// Memory: every structure is fixed-size and lives on the calling activity, in
// the style of BookFusionSyncClient. Borrowing limits are small (most library
// systems allow 5-20 concurrent loans), so the whole list is a few KB and
// pagination is unnecessary.

#include <stdint.h>

#include <cstddef>

// One borrowed title. 216 bytes.
struct LibbyLoan {
  char id[24] = {};      // loan id, used in /card/<cardId>/loan/<id>
  char cardId[24] = {};  // library card the loan belongs to
  char title[72] = {};
  char author[48] = {};
  char format[32] = {};   // e.g. "ebook-epub-adobe"; empty if no Adobe EPUB
  int64_t expiresAt = 0;  // epoch seconds; 0 when Libby gave no date

  // False for audiobooks, magazines and anything with no Adobe EPUB edition.
  // Those rows are still listed, so the reason a title cannot be sent is
  // visible rather than the title silently missing.
  bool sendable() const { return format[0] != '\0'; }
};

struct LibbyLoanList {
  // A borrowing limit this high is rare; extra loans are dropped with a log
  // rather than growing the activity's footprint.
  static constexpr int MAX_LOANS = 24;
  LibbyLoan loans[MAX_LOANS];
  int count = 0;
  int dropped = 0;  // loans beyond MAX_LOANS, for an honest "showing N of M"
};

class LibbyClient {
 public:
  enum Error {
    OK = 0,
    NO_IDENTITY,      // no libby.json -- link the account in the web UI first
    NO_CREDENTIAL,    // no content.key -- authorise the reader in the web UI
    NETWORK_ERROR,    // TLS/HTTP failure
    AUTH_FAILED,      // Libby rejected the chip even after a refresh
    SERVER_ERROR,     // unexpected status
    JSON_ERROR,       // malformed reply
    NO_ADOBE_FORMAT,  // the loan has no Adobe EPUB edition
    SD_ERROR,         // could not write to the card
    NOT_AUTHORISED,   // the distributor refused this reader (ADEPT)
    FULFIL_FAILED,    // the ADEPT fulfilment exchange failed
  };

  // Human-readable, for the error screen. Never null.
  static const char* errorText(Error e);

  // Loads the saved chip from SD. Cheap; call before anything else. Everything
  // below returns NO_IDENTITY until this has succeeded once.
  static Error loadIdentity();
  static bool hasIdentity();

  // True when /.crosspoint/content.key exists, i.e. the reader has been
  // authorised. Checked separately from the chip so the UI can say which of the
  // two setup steps is missing.
  static bool hasCredential();

  // GET /chip/sync, keeping only the loan fields.
  //
  // The full sync payload carries every card, hold and library the account
  // touches and is far too large to hold in RAM, so it is streamed to the card
  // and parsed back through an ArduinoJson filter that discards everything
  // else. The temporary file is deleted before returning -- it is account
  // metadata and does not belong on the card.
  static Error fetchLoans(LibbyLoanList& out);

  // Ask Libby to fulfil a loan and write the resulting token to `acsmPath`.
  // This is only the Libby half; the ADEPT exchange that turns it into a book
  // is AdeptClient::fulfil().
  static Error fetchAcsm(const LibbyLoan& loan, const char* acsmPath);

  // Extend a loan. Libby refuses outside the renewal window or when someone is
  // waiting, which surfaces as SERVER_ERROR.
  static Error renewLoan(const LibbyLoan& loan);

  // Where a sent loan landed. Both UIs share /.crosspoint/libby-books.json, so a
  // book sent from the browser can be renewed on the device and vice versa.
  //
  // This exists because a renewal rewrites only the .rights sidecar and reuses
  // the EPUB already on the card -- for a 100MB+ title that is the difference
  // between seconds and a re-download. Without the mapping there is no way to
  // know which file to rewrite.
  static bool rememberBook(const char* loanId, const char* bookPath);
  static bool lookupBook(const char* loanId, char* outPath, size_t outLen);

  // Copy the largest cover URL OverDrive publishes for this loan into `outUrl`.
  //
  // The artwork is fetched on demand rather than kept on LibbyLoan, because a
  // loan's `id` IS the OverDrive title id -- every /card/<card>/loan/<id>
  // endpoint keys on it -- so the list does not have to carry MAX_LOANS x ~128
  // bytes of URL for the one title that is about to be sent.
  //
  // Uses OverDrive's public catalogue, not Libby: no chip, no card, no
  // Authorization header. Best-effort by design -- a cover is decoration, so
  // this returns false and logs rather than failing a send.
  static bool fetchCoverUrl(const LibbyLoan& loan, char* outUrl, size_t outLen);

  // Give a loan back. Note this does NOT stop an already-downloaded copy from
  // opening: the reader checks the due date baked into the .rights sidecar and
  // never asks Libby anything. Delete the book to remove it now.
  static Error returnLoan(const LibbyLoan& loan);

  // Drop the keep-alive TLS connection so its heap is returned before WiFi is
  // torn down. The next call reconnects transparently.
  static void closeConnection();
};
