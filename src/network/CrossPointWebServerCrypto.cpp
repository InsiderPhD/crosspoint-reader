#include <DevicePolicy.h>

#if CROSSPOINT_SD_PLUGINS  // SD-card plugins: PSRAM boards only, see lib/DevicePolicy

// POST /api/crypto — generic, stateless crypto primitives for browser plugins,
// keyed by "op" with base64 fields in and out. A plugin reaches for these when
// a service's protocol needs something the browser's crypto.subtle cannot do
// from a page served over plain HTTP (crypto.subtle is unavailable outside a
// secure context on most browsers), or needs a raw RSA operation.
//
// Implemented straight on wolfCrypt (already linked for TLS) plus mbedTLS for
// RSA key generation. Nothing here is tied to any service: the ops are the
// textbook primitives (random, SHA-1/SHA-256, HMAC-SHA256, AES-128-CBC, RSA
// PKCS#1 v1.5 encrypt to a certificate, raw RSA sign, PKCS#12 unpack).
//
// The DER helpers are adapted from freeink-sdk's WolfsslCrypto (MIT); the SDK
// library itself is not linked because it bundles a decrypt-on-read book
// path this firmware must not carry.

#include <ArduinoJson.h>
#include <Logging.h>
#include <base64.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/asn.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/pkcs12.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/rsa.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointWebServer.h"
#include "Memory.h"

namespace {

std::string b64encode(const uint8_t* data, size_t len) { return base64::encode(data, len).c_str(); }
std::string b64encode(const std::vector<uint8_t>& v) { return b64encode(v.data(), v.size()); }

// RsaKey is ~8.4 KB with wolfSSL's embedded math configuration: too big for the
// loop task's stack, so it lives on the heap (PSRAM on these boards).
class ScopedRsaKey {
 public:
  ScopedRsaKey() : key_(static_cast<RsaKey*>(malloc(sizeof(RsaKey)))) {
    if (key_ && wc_InitRsaKey(key_, nullptr) != 0) {
      free(key_);
      key_ = nullptr;
    }
  }
  ~ScopedRsaKey() {
    if (key_) {
      wc_FreeRsaKey(key_);
      free(key_);
    }
  }
  ScopedRsaKey(const ScopedRsaKey&) = delete;
  ScopedRsaKey& operator=(const ScopedRsaKey&) = delete;
  RsaKey* get() const { return key_; }

 private:
  RsaKey* key_;
};

class ScopedRng {
 public:
  ScopedRng() : ok_(wc_InitRng(&rng_) == 0) {}
  ~ScopedRng() {
    if (ok_) wc_FreeRng(&rng_);
  }
  ScopedRng(const ScopedRng&) = delete;
  ScopedRng& operator=(const ScopedRng&) = delete;
  bool ok() const { return ok_; }
  WC_RNG* get() { return &rng_; }

 private:
  WC_RNG rng_;
  bool ok_;
};

// --- tiny DER helpers -------------------------------------------------------

// Bytes DER uses to encode `len` in minimal definite form. Must stay in
// lockstep with derWriteLen.
size_t derLenLen(size_t len) {
  if (len < 128) return 1;
  if (len < 0x100) return 2;
  return 3;  // key material never exceeds 64 KB
}

uint8_t* derWriteLen(uint8_t* p, size_t len) {
  if (len < 128) {
    *p++ = static_cast<uint8_t>(len);
  } else if (len < 0x100) {
    *p++ = 0x81;
    *p++ = static_cast<uint8_t>(len);
  } else {
    *p++ = 0x82;
    *p++ = static_cast<uint8_t>(len >> 8);
    *p++ = static_cast<uint8_t>(len);
  }
  return p;
}

// Wraps a PKCS#1 RSAPrivateKey blob as PKCS#8 PrivateKeyInfo (rsaEncryption).
void wrapPkcs8(const uint8_t* pkcs1, size_t pkcs1Len, std::vector<uint8_t>* out) {
  static constexpr uint8_t kAlgId[18] = {0x02, 0x01, 0x00, 0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86,
                                         0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01, 0x05, 0x00};
  const size_t contentLen = sizeof(kAlgId) + 1 + derLenLen(pkcs1Len) + pkcs1Len;
  out->clear();
  out->reserve(2 + derLenLen(contentLen) + contentLen);
  out->push_back(0x30);
  uint8_t scratch[4];
  uint8_t* end = derWriteLen(scratch, contentLen);
  out->insert(out->end(), scratch, end);
  out->insert(out->end(), kAlgId, kAlgId + sizeof(kAlgId));
  out->push_back(0x04);
  end = derWriteLen(scratch, pkcs1Len);
  out->insert(out->end(), scratch, end);
  out->insert(out->end(), pkcs1, pkcs1 + pkcs1Len);
}

// Locates the SubjectPublicKeyInfo inside an X.509 cert by scanning for the
// rsaEncryption algorithm identifier and backing up to the enclosing
// SEQUENCE. Fallback for certificates wolfSSL's parser rejects.
const uint8_t* spkiFromX509(const uint8_t* cert, size_t len, size_t* spkiLen) {
  static constexpr uint8_t kRsaAlgId[13] = {0x30, 0x0D, 0x06, 0x09, 0x2A, 0x86, 0x48,
                                            0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01};
  for (size_t i = 0; i + sizeof(kRsaAlgId) + 4 < len; i++) {
    if (memcmp(cert + i, kRsaAlgId, sizeof(kRsaAlgId)) != 0) continue;
    for (int hdr = 3; hdr <= 4; hdr++) {
      if (i < static_cast<size_t>(hdr) || cert[i - hdr] != 0x30) continue;
      size_t total = 0;
      if (hdr == 3 && cert[i - 2] == 0x81) {
        total = 3 + cert[i - 1];
      } else if (hdr == 4 && cert[i - 3] == 0x82) {
        total = 4 + ((static_cast<size_t>(cert[i - 2]) << 8) | cert[i - 1]);
      }
      if (total == 0) continue;
      const uint8_t* spki = cert + i - hdr;
      if (spki + total <= cert + len) {
        *spkiLen = total;
        return spki;
      }
    }
  }
  return nullptr;
}

bool decodePrivateKey(const uint8_t* pkcs8Der, size_t pkcs8Len, RsaKey* key) {
  // ToTraditional converts PKCS#8 to PKCS#1 in place; hand it a scratch copy.
  std::vector<uint8_t> buf(pkcs8Der, pkcs8Der + pkcs8Len);
  int len = ToTraditional(buf.data(), static_cast<word32>(buf.size()));
  if (len < 0) len = static_cast<int>(pkcs8Len);  // maybe already PKCS#1
  word32 idx = 0;
  return wc_RsaPrivateKeyDecode(buf.data(), &idx, key, static_cast<word32>(len)) == 0;
}

// --- primitives -------------------------------------------------------------

// RSA-1024 keypair as (SPKI public, PKCS#8 private). mbedTLS rather than
// wolfSSL's key generation: freeink-sdk found wolfSSL 5.7.2's TFM prime search
// unstable on the ESP32 family, and ESP-IDF's mbedTLS ships with keygen on.
int mbedtlsRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

bool rsaGenerate(std::vector<uint8_t>* spki, std::vector<uint8_t>* pkcs8, std::string* err) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
  if (rc == 0) rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtlsRng, nullptr, 1024, 65537);

  // Off the stack: ~1.2KB of DER scratch on the loop task's 8KB stack.
  auto pubBuf = makeUniqueNoThrow<uint8_t[]>(192);
  auto privBuf = makeUniqueNoThrow<uint8_t[]>(1024);
  int pubLen = 0;
  int privLen = 0;
  if (rc == 0 && pubBuf && privBuf) {
    // mbedTLS writes DER at the END of the buffer.
    pubLen = mbedtls_pk_write_pubkey_der(&key, pubBuf.get(), 192);
    if (pubLen > 0) privLen = mbedtls_pk_write_key_der(&key, privBuf.get(), 1024);
  }
  const bool ok = pubLen > 0 && privLen > 0;
  if (ok) {
    spki->assign(pubBuf.get() + 192 - pubLen, pubBuf.get() + 192);
    wrapPkcs8(privBuf.get() + 1024 - privLen, static_cast<size_t>(privLen), pkcs8);
  } else {
    *err = "mbedTLS RSA keygen rc=" + std::to_string(rc) + " pub=" + std::to_string(pubLen) +
           " priv=" + std::to_string(privLen);
  }
  mbedtls_pk_free(&key);
  return ok;
}

// RSAES-PKCS1-v1_5 to the public key of an X.509 certificate (DER).
bool rsaPublicEncrypt(WC_RNG* rng, const uint8_t* certDer, size_t certLen, const uint8_t* in, size_t inLen,
                      std::vector<uint8_t>* out, std::string* err) {
  const uint8_t* spki = nullptr;
  size_t spkiLen = 0;
  auto spkiBuf = makeUniqueNoThrow<uint8_t[]>(1024);
  if (!spkiBuf) {
    *err = "no memory";
    return false;
  }
  {
    DecodedCert cert;
    wc_InitDecodedCert(&cert, certDer, static_cast<word32>(certLen), nullptr);
    const int rc = wc_ParseCert(&cert, CERT_TYPE, NO_VERIFY, nullptr);
    if (rc == 0) {
      word32 n = 1024;
      if (wc_GetPubKeyDerFromCert(&cert, spkiBuf.get(), &n) == 0) {
        spki = spkiBuf.get();
        spkiLen = n;
      }
    } else {
      *err = "wc_ParseCert rc=" + std::to_string(rc);
    }
    wc_FreeDecodedCert(&cert);
  }
  if (!spki) {
    spki = spkiFromX509(certDer, certLen, &spkiLen);
    if (!spki) {
      *err += "; spki scan failed";
      return false;
    }
  }

  ScopedRsaKey key;
  if (!key.get()) {
    *err = "wc_InitRsaKey failed";
    return false;
  }
  // Accepts a bare PKCS#1 RSAPublicKey or a full SubjectPublicKeyInfo.
  word32 idx = 0;
  if (wc_RsaPublicKeyDecode(spki, &idx, key.get(), static_cast<word32>(spkiLen)) != 0) {
    *err = "wc_RsaPublicKeyDecode failed";
    return false;
  }
  const int modBytes = wc_RsaEncryptSize(key.get());
  if (modBytes <= 0 || modBytes > 512) {
    *err = "unsupported key size";
    return false;
  }
  out->assign(static_cast<size_t>(modBytes), 0);
  const int n = wc_RsaPublicEncrypt(in, static_cast<word32>(inLen), out->data(), static_cast<word32>(out->size()),
                                    key.get(), rng);
  if (n <= 0) {
    out->clear();
    // RSA_BUFFER_E (-131) means the input did not fit the key: PKCS#1 v1.5
    // leaves keysize-11 bytes. Say so in bytes the caller controls.
    *err = "wc_RsaPublicEncrypt rc=" + std::to_string(n) + " modbits=" + std::to_string(modBytes * 8);
    if (n == -131) {
      *err += "; input too long for this key: PKCS#1 v1.5 fits " + std::to_string(modBytes - 11) + " bytes, got " +
              std::to_string(inLen);
    }
    return false;
  }
  out->resize(static_cast<size_t>(n));
  return true;
}

// PKCS#1 type-1 pad of a 20-byte hash followed by the raw private operation
// (m^d mod n) — a 1024-bit signature over a SHA-1 digest with no DigestInfo.
bool rsaSignRaw(WC_RNG* rng, const uint8_t* pkcs8Der, size_t pkcs8Len, const uint8_t hash[20], uint8_t out[128],
                std::string* err) {
  ScopedRsaKey key;
  if (!key.get()) {
    *err = "wc_InitRsaKey failed";
    return false;
  }
  if (!decodePrivateKey(pkcs8Der, pkcs8Len, key.get())) {
    *err = "private key decode failed";
    return false;
  }
  if (wc_RsaEncryptSize(key.get()) != 128) {
    *err = "sign needs a 1024-bit key";
    return false;
  }
  uint8_t padded[128];
  memset(padded, 0xFF, sizeof(padded));
  padded[0] = 0x00;
  padded[1] = 0x01;
  padded[sizeof(padded) - 20 - 1] = 0x00;
  memcpy(padded + sizeof(padded) - 20, hash, 20);
  word32 outLen = 128;
  // RSA_PRIVATE_ENCRYPT is m^d; RSA_PRIVATE (==RSA_PUBLIC_DECRYPT) would use e.
  const int rc = wc_RsaFunction(padded, sizeof(padded), out, &outLen, RSA_PRIVATE_ENCRYPT, key.get(), rng);
  if (rc != 0 || outLen != 128) {
    *err = "wc_RsaFunction rc=" + std::to_string(rc);
    return false;
  }
  return true;
}

// Unpacks a PKCS#12 bundle into (PKCS#8 private key, X.509 cert).
bool pkcs12Extract(const uint8_t* p12, size_t len, const std::string& password, std::vector<uint8_t>* keyPkcs8,
                   std::vector<uint8_t>* certDer, std::string* err) {
  WC_PKCS12* bundle = wc_PKCS12_new();
  if (!bundle) {
    *err = "wc_PKCS12_new failed";
    return false;
  }
  const int d2i = wc_d2i_PKCS12(p12, static_cast<word32>(len), bundle);
  if (d2i != 0) {
    *err = "wc_d2i_PKCS12 rc=" + std::to_string(d2i) + " len=" + std::to_string(len);
    wc_PKCS12_free(bundle);
    return false;
  }

  byte* key = nullptr;
  byte* cert = nullptr;
  word32 keyLen = 0, certLen = 0;
  // Ask for the unmatched certificates too: wc_PKCS12_parse only fills `cert`
  // with a bag it could pair to the private key, and frees the rest when this
  // is NULL. A parse hiccup on the leaf then looks like a bundle with no cert.
  WC_DerCertList* caList = nullptr;
  const int rc = wc_PKCS12_parse(bundle, password.c_str(), &key, &keyLen, &cert, &certLen, &caList);
  wc_PKCS12_free(bundle);

  std::vector<uint8_t> certOut;
  size_t caCount = 0;
  if (cert && certLen > 0) {
    certOut.assign(cert, cert + certLen);
  } else {
    for (WC_DerCertList* node = caList; node != nullptr; node = node->next) {
      caCount++;
      if (certOut.empty() && node->buffer && node->bufferSz > 0) {
        certOut.assign(node->buffer, node->buffer + node->bufferSz);
      }
    }
  }
  if (caList) wc_FreeCertList(caList, NULL);

  bool ok = rc >= 0 && key && !certOut.empty();
  if (!ok) {
    // rc<0 with a valid bundle usually means the passphrase is wrong.
    *err = "wc_PKCS12_parse rc=" + std::to_string(rc) + " keyLen=" + std::to_string(keyLen) +
           " certLen=" + std::to_string(certLen) + " caCerts=" + std::to_string(caCount);
  } else {
    wrapPkcs8(key, keyLen, keyPkcs8);  // the parsed key is PKCS#1
    *certDer = std::move(certOut);
  }
  if (key) XFREE(key, NULL, DYNAMIC_TYPE_PUBLIC_KEY);
  if (cert) XFREE(cert, NULL, DYNAMIC_TYPE_PKCS);
  return ok;
}

}  // namespace

// POST /api/crypto {op, ...base64 fields...} -> {data|public|private|key|cert} or {error}
void CrossPointWebServer::handleCrypto() {
  JsonDocument req;
  if (!readJsonBody(req)) return;
  const std::string op = req["op"] | "";

  // The asymmetric ops hold wolfSSL working sets measured in kilobytes for
  // seconds. The WebSocket server and discovery UDP cannot answer anyone while
  // this request is served anyway, so their buffers go to crypto headroom —
  // the same trade /api/relay and /api/fetch make. Cheap symmetric ops are
  // left alone: the teardown costs more than a hash needs.
  const bool heavyOp = op == "keygen" || op == "pkcs12" || op == "pubencrypt" || op == "sign";
  if (heavyOp) suspendTransferServices();
  ScopedCleanup resumeServices{[this, heavyOp] {
    if (heavyOp) resumeTransferServices();
  }};

  auto dec = [&](const char* field) -> std::string {
    const char* v = req[field].as<const char*>();
    if (!v) return std::string();
    const size_t encodedLen = strlen(v);
    // Crypto inputs (keys, certs, small payloads) are a few KB at most. The cap
    // stops a LAN client posting a multi-megabyte value into the fallible
    // resize below (-fno-exceptions).
    static constexpr size_t kMaxCryptoField = 64 * 1024;
    if (encodedLen > kMaxCryptoField) return std::string();
    const size_t needed = (encodedLen * 3) / 4 + 3;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < needed + 2048) {
      LOG_ERR("WEB", "Crypto field too large for heap: %u bytes", (unsigned)needed);
      return std::string();
    }
    std::string decoded;
    decoded.resize(needed);
    word32 decodedLen = static_cast<word32>(needed);
    if (Base64_Decode(reinterpret_cast<const byte*>(v), static_cast<word32>(encodedLen),
                      reinterpret_cast<byte*>(decoded.data()), &decodedLen) != 0) {
      return std::string();
    }
    decoded.resize(decodedLen);
    return decoded;
  };

  JsonDocument resp;
  std::string err;
  LOG_DBG("WEB", "Crypto op '%s': heap %u, max block %u", op.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());

  ScopedRng rng;
  if (!rng.ok()) {
    server->send(500, "application/json", "{\"error\":\"rng init failed\"}");
    return;
  }

  if (op == "random") {
    static constexpr int kMaxRandomBytes = 4096;
    const int n = std::clamp(static_cast<int>(req["len"] | 16), 0, kMaxRandomBytes);
    std::vector<uint8_t> out(static_cast<size_t>(n));
    if (!out.empty()) wc_RNG_GenerateBlock(rng.get(), out.data(), static_cast<word32>(out.size()));
    resp["data"] = b64encode(out);
  } else if (op == "sha1") {
    const std::string d = dec("data");
    uint8_t h[WC_SHA_DIGEST_SIZE];
    wc_ShaHash(reinterpret_cast<const byte*>(d.data()), static_cast<word32>(d.size()), h);
    resp["data"] = b64encode(h, sizeof(h));
  } else if (op == "sha256") {
    const std::string d = dec("data");
    uint8_t h[WC_SHA256_DIGEST_SIZE];
    wc_Sha256Hash(reinterpret_cast<const byte*>(d.data()), static_cast<word32>(d.size()), h);
    resp["data"] = b64encode(h, sizeof(h));
  } else if (op == "hmac") {
    // HMAC-SHA256 over data with key.
    const std::string k = dec("key"), d = dec("data");
    Hmac hmac;
    uint8_t h[WC_SHA256_DIGEST_SIZE];
    if (wc_HmacInit(&hmac, nullptr, INVALID_DEVID) != 0 ||
        wc_HmacSetKey(&hmac, WC_SHA256, reinterpret_cast<const byte*>(k.data()), static_cast<word32>(k.size())) != 0 ||
        wc_HmacUpdate(&hmac, reinterpret_cast<const byte*>(d.data()), static_cast<word32>(d.size())) != 0 ||
        wc_HmacFinal(&hmac, h) != 0) {
      resp["error"] = "hmac failed";
    } else {
      resp["data"] = b64encode(h, sizeof(h));
    }
    wc_HmacFree(&hmac);
  } else if (op == "aesenc" || op == "aesdec") {
    const std::string k = dec("key"), iv = dec("iv"), d = dec("data");
    if (k.size() != 16 || iv.size() != 16) {
      resp["error"] = "key/iv must be 16 bytes";
    } else if (op == "aesenc") {
      // PKCS#7 pad first (wolfSSL does no padding).
      const size_t paddedLen = ((d.size() / 16) + 1) * 16;
      std::vector<uint8_t> padded(paddedLen, static_cast<uint8_t>(paddedLen - d.size()));
      memcpy(padded.data(), d.data(), d.size());
      std::vector<uint8_t> out(paddedLen);
      Aes aes;
      if (wc_AesSetKey(&aes, reinterpret_cast<const byte*>(k.data()), 16, reinterpret_cast<const byte*>(iv.data()),
                       AES_ENCRYPTION) == 0 &&
          wc_AesCbcEncrypt(&aes, out.data(), padded.data(), static_cast<word32>(paddedLen)) == 0) {
        resp["data"] = b64encode(out);
      } else {
        resp["error"] = "aesenc failed";
      }
    } else if (d.size() % 16 != 0) {
      resp["error"] = "data not block-aligned";
    } else {
      // Raw blocks back; the caller strips its own padding.
      std::vector<uint8_t> out(d.size());
      Aes aes;
      if (wc_AesSetKey(&aes, reinterpret_cast<const byte*>(k.data()), 16, reinterpret_cast<const byte*>(iv.data()),
                       AES_DECRYPTION) == 0 &&
          wc_AesCbcDecrypt(&aes, out.data(), reinterpret_cast<const byte*>(d.data()), static_cast<word32>(d.size())) ==
              0) {
        resp["data"] = b64encode(out);
      } else {
        resp["error"] = "aesdec failed";
      }
    }
  } else if (op == "keygen") {
    std::vector<uint8_t> spki, pkcs8;
    if (rsaGenerate(&spki, &pkcs8, &err)) {
      resp["public"] = b64encode(spki);
      resp["private"] = b64encode(pkcs8);
    } else {
      resp["error"] = "keygen failed: " + err;
    }
  } else if (op == "pubencrypt") {
    const std::string cert = dec("cert"), d = dec("data");
    std::vector<uint8_t> out;
    if (rsaPublicEncrypt(rng.get(), reinterpret_cast<const uint8_t*>(cert.data()), cert.size(),
                         reinterpret_cast<const uint8_t*>(d.data()), d.size(), &out, &err)) {
      resp["data"] = b64encode(out);
    } else {
      resp["error"] = "pubencrypt failed: " + err + " (cert " + std::to_string(cert.size()) + "B, data " +
                      std::to_string(d.size()) + "B)";
    }
  } else if (op == "sign") {
    const std::string priv = dec("private"), h = dec("hash");
    if (h.size() != 20) {
      resp["error"] = "hash must be 20 bytes";
    } else {
      uint8_t sig[128];
      if (rsaSignRaw(rng.get(), reinterpret_cast<const uint8_t*>(priv.data()), priv.size(),
                     reinterpret_cast<const uint8_t*>(h.data()), sig, &err)) {
        resp["data"] = b64encode(sig, sizeof(sig));
      } else {
        resp["error"] = "sign failed: " + err;
      }
    }
  } else if (op == "pkcs12") {
    const std::string pw = req["password"] | "";
    const std::string bundle = dec("data");
    // The decoded bundle no longer depends on the request document; reclaim
    // its base64 string before the KDF and certificate parsing begin.
    req.clear();
    req.shrinkToFit();
    releaseRequestArguments();
    std::vector<uint8_t> key, cert;
    if (bundle.empty()) {
      resp["error"] = "pkcs12 failed: missing or invalid data";
    } else if (pkcs12Extract(reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size(), pw, &key, &cert, &err)) {
      resp["key"] = b64encode(key);
      resp["cert"] = b64encode(cert);
    } else {
      resp["error"] = "pkcs12 failed: " + err;
    }
  } else {
    resp["error"] = "unknown op";
  }

  String out;
  serializeJson(resp, out);
  server->send(200, "application/json", out);
}

#endif  // CROSSPOINT_SD_PLUGINS
