#pragma once

// ADEPT fulfilment, on device.
//
// Turns a fulfilment token (the .acsm Libby hands out) into a book on the SD
// card: authenticate to the library's distributor, exchange the token for a
// download URL and a licence, write the licence beside the book as a .rights
// sidecar, and stream the EPUB down.
//
// SCOPE, and why it is this shape:
//
// Fulfilment needs only the crypto this firmware already links -- SHA-1 and a
// raw RSA private-key signature (Crypto.h's "read path" half). ACTIVATION is a
// different matter: it needs RSA key generation, PKCS#1 v1.5 encryption and
// PKCS#12 parsing, none of which are compiled in (see the
// FREEINK_CONTENT_CLIENT_CRYPTO fence in WolfsslCrypto.cpp). That is the line
// between what runs here and what stays in the web UI, and it is not arbitrary:
// it falls exactly where the crypto does.
//
// So this reads the signing material the browser produced:
//   /.crosspoint/libby-session.json   signing key + cert, user/device uuids
// and refuses with NOT_SET_UP if it is absent.
//
// Memory: the request documents are a few KB of XML built as strings; the book
// itself is streamed to SD and never held. The one notable cost is the
// canonicalisation buffer, which is proportional to the request, not the book.

#include <stdint.h>

#include <cstddef>
#include <string>

class AdeptClient {
 public:
  enum Error {
    OK = 0,
    NOT_SET_UP,        // no libby-session.json -- authorise in the web UI first
    BAD_TOKEN,         // the .acsm is not a fulfilment token
    NETWORK_ERROR,     // TLS/HTTP failure
    DISTRIBUTOR_AUTH,  // the operator refused this reader
    SERVER_ERROR,      // the service returned an ADEPT <error>
    SIGN_FAILED,       // could not sign the request (bad/missing signing key)
    SD_ERROR,          // could not write the book or its licence
    DOWNLOAD_FAILED,   // the book transfer did not complete
  };

  static const char* errorText(Error e);

  // Progress during the long steps, so the UI can show something truthful.
  // `phase` is a short label ("Authenticating…", "Downloading…"); `done`/`total`
  // are bytes and are 0 outside the transfer. Return false to abort.
  using ProgressFn = bool (*)(void* ctx, const char* phase, size_t done, size_t total);

  struct Result {
    char title[72] = {};      // from the fulfilment reply, sanitised for a filename
    char destPath[128] = {};  // where the .epub landed
    size_t bytes = 0;
  };

  // Load the browser-written session. Call once before fulfil(); everything
  // returns NOT_SET_UP until it succeeds.
  static Error loadSession();
  static bool hasSession();

  // Fulfil `acsmPath` into `destDir`, naming the file after the title in the
  // reply and avoiding collisions. Writes `<book>.epub.rights` first, so a
  // transfer interrupted after the licence lands can be resumed by re-sending
  // without re-authorising.
  static Error fulfil(const char* acsmPath, const char* destDir, Result& out, ProgressFn progress = nullptr,
                      void* progressCtx = nullptr);

  // Refresh only the licence for a book already on the card -- what a renewal
  // needs. Rewrites `<bookPath>.rights` and does not re-download the EPUB.
  static Error refreshRights(const char* acsmPath, const char* bookPath, ProgressFn progress = nullptr,
                             void* progressCtx = nullptr);

  static void closeConnection();
};
