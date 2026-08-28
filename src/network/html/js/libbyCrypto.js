// Pure-JavaScript crypto for the Libby page.
//
// WHY NOT crypto.subtle: the device serves this page over plain http:// on the
// local network, and browsers expose SubtleCrypto only in a secure context
// (https:, or localhost). http://crosspoint.local/ and http://192.168.x.x/ are
// neither, so window.crypto.subtle is undefined here. window.crypto.getRandomValues
// IS available -- it is not gated behind secure context -- so entropy comes from
// the browser and only the algorithms are implemented below.
//
// WHY NOT ON THE DEVICE: every primitive here exists in wolfSSL, which the
// firmware already links for TLS. Calling it would mean an /api/crypto endpoint
// plus the WOLFSSL_KEY_GEN and WC_RC2 build options, i.e. more flash and more
// heap during activation on a board that has ~380KB total. Doing it in the
// browser costs the device nothing at all: this file is a gzipped constant in
// flash, and the work happens on a machine with gigabytes.
//
// SCOPE: exactly what Adobe's ADEPT activation and fulfilment need.
//   SHA-1                  request signing, PKCS#12 KDF, device fingerprint
//   AES-128-CBC            device-key wrap of the private keys
//   RSA (BigInt)           PKCS#1 v1.5 encrypt/sign, 1024-bit key generation
//   RC2-40 / 3DES-CBC      the two PBE ciphers a PKCS#12 signing bundle uses
//   ASN.1 DER              SPKI/PKCS#8 emit, X.509 + PKCS#12 parse
//
// These are standard, fully specified algorithms; the notes on each say which
// document they follow so they can be checked against it.

(function (global) {
  'use strict';

  // ==========================================================================
  // bytes / base64 / hex
  // ==========================================================================

  const te = new TextEncoder();

  function utf8(str) {
    return te.encode(str);
  }

  function concat(arrays) {
    let total = 0;
    for (const a of arrays) total += a.length;
    const out = new Uint8Array(total);
    let at = 0;
    for (const a of arrays) {
      out.set(a, at);
      at += a.length;
    }
    return out;
  }

  function bytesToB64(bytes) {
    let bin = '';
    // Chunked: String.fromCharCode.apply blows the argument limit on large inputs.
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(bin);
  }

  function b64ToBytes(b64) {
    const bin = atob(String(b64).replace(/\s+/g, ''));
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function bytesToHex(bytes) {
    let s = '';
    for (const b of bytes) s += b.toString(16).padStart(2, '0');
    return s;
  }

  function randomBytes(n) {
    const out = new Uint8Array(n);
    global.crypto.getRandomValues(out);
    return out;
  }

  // ==========================================================================
  // SHA-1  (FIPS 180-4)
  // ==========================================================================
  //
  // Kept as a raw implementation rather than a hashing library: ADEPT signs a
  // SHA-1 of its own XML canonicalization, and the PKCS#12 KDF calls SHA-1 in a
  // loop, so both need direct access to the digest of arbitrary bytes.

  function sha1(bytes) {
    const ml = bytes.length;
    // Pad to a multiple of 64: 0x80, zeros, then the 64-bit big-endian bit length.
    const withPad = new Uint8Array(((ml + 8) >> 6 << 6) + 64);
    withPad.set(bytes);
    withPad[ml] = 0x80;
    const dv = new DataView(withPad.buffer);
    // Lengths here are far below 2^32 bytes, so the high word is always zero.
    dv.setUint32(withPad.length - 4, (ml << 3) >>> 0, false);
    dv.setUint32(withPad.length - 8, Math.floor(ml / 0x20000000), false);

    let h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476, h4 = 0xc3d2e1f0;
    const w = new Int32Array(80);

    for (let off = 0; off < withPad.length; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getInt32(off + i * 4, false);
      for (let i = 16; i < 80; i++) {
        const n = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (n << 1) | (n >>> 31);
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4;
      for (let i = 0; i < 80; i++) {
        let f, k;
        if (i < 20) {
          f = (b & c) | (~b & d);
          k = 0x5a827999;
        } else if (i < 40) {
          f = b ^ c ^ d;
          k = 0x6ed9eba1;
        } else if (i < 60) {
          f = (b & c) | (b & d) | (c & d);
          k = 0x8f1bbcdc;
        } else {
          f = b ^ c ^ d;
          k = 0xca62c1d6;
        }
        const t = (((a << 5) | (a >>> 27)) + f + e + k + w[i]) | 0;
        e = d;
        d = c;
        c = (b << 30) | (b >>> 2);
        b = a;
        a = t;
      }
      h0 = (h0 + a) | 0;
      h1 = (h1 + b) | 0;
      h2 = (h2 + c) | 0;
      h3 = (h3 + d) | 0;
      h4 = (h4 + e) | 0;
    }

    const out = new Uint8Array(20);
    new DataView(out.buffer).setInt32(0, h0, false);
    new DataView(out.buffer).setInt32(4, h1, false);
    new DataView(out.buffer).setInt32(8, h2, false);
    new DataView(out.buffer).setInt32(12, h3, false);
    new DataView(out.buffer).setInt32(16, h4, false);
    return out;
  }

  // ==========================================================================
  // AES-128  (FIPS 197), CBC mode
  // ==========================================================================

  const AES_SBOX = new Uint8Array(256);
  const AES_INV_SBOX = new Uint8Array(256);
  const AES_RCON = new Uint8Array([0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36]);

  (function buildAesTables() {
    // Multiplicative inverse in GF(2^8) followed by the affine transform.
    const p = new Uint8Array(256);
    const l = new Uint8Array(256);
    let x = 1;
    for (let i = 0; i < 255; i++) {
      p[i] = x;
      l[x] = i;
      x ^= (x << 1) ^ (x & 0x80 ? 0x11b : 0);
      x &= 0xff;
    }
    // The exponent wraps mod 255: p[] holds 3^0..3^254, so a = 1 (l[a] == 0)
    // would index p[255], which the loop above never writes. Without the mod,
    // inv(1) reads that hole as 0 and S[0x01] comes out 0x63 instead of 0x7c --
    // one wrong entry, which also clobbers two entries of the inverse table.
    const inv = (a) => (a === 0 ? 0 : p[(255 - l[a]) % 255]);
    for (let i = 0; i < 256; i++) {
      let s = inv(i);
      let v = s;
      for (let j = 0; j < 4; j++) {
        s = ((s << 1) | (s >>> 7)) & 0xff;
        v ^= s;
      }
      v ^= 0x63;
      AES_SBOX[i] = v;
      AES_INV_SBOX[v] = i;
    }
  })();

  function xtime(a) {
    return ((a << 1) ^ (a & 0x80 ? 0x1b : 0)) & 0xff;
  }

  function gmul(a, b) {
    let r = 0;
    while (b) {
      if (b & 1) r ^= a;
      a = xtime(a);
      b >>= 1;
    }
    return r & 0xff;
  }

  function aesExpandKey(key) {
    // AES-128 only: 4-word key, 11 round keys.
    const rk = new Uint8Array(176);
    rk.set(key);
    for (let i = 4; i < 44; i++) {
      let t0 = rk[(i - 1) * 4], t1 = rk[(i - 1) * 4 + 1], t2 = rk[(i - 1) * 4 + 2], t3 = rk[(i - 1) * 4 + 3];
      if (i % 4 === 0) {
        const tmp = t0;
        t0 = AES_SBOX[t1] ^ AES_RCON[i / 4 - 1];
        t1 = AES_SBOX[t2];
        t2 = AES_SBOX[t3];
        t3 = AES_SBOX[tmp];
      }
      rk[i * 4] = rk[(i - 4) * 4] ^ t0;
      rk[i * 4 + 1] = rk[(i - 4) * 4 + 1] ^ t1;
      rk[i * 4 + 2] = rk[(i - 4) * 4 + 2] ^ t2;
      rk[i * 4 + 3] = rk[(i - 4) * 4 + 3] ^ t3;
    }
    return rk;
  }

  function aesEncryptBlock(rk, block) {
    const s = block.slice();
    for (let i = 0; i < 16; i++) s[i] ^= rk[i];
    for (let round = 1; round <= 10; round++) {
      for (let i = 0; i < 16; i++) s[i] = AES_SBOX[s[i]];
      shiftRows(s);
      if (round !== 10) mixColumns(s);
      for (let i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
    }
    return s;
  }

  function aesDecryptBlock(rk, block) {
    const s = block.slice();
    for (let i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (let round = 9; round >= 0; round--) {
      invShiftRows(s);
      for (let i = 0; i < 16; i++) s[i] = AES_INV_SBOX[s[i]];
      for (let i = 0; i < 16; i++) s[i] ^= rk[round * 16 + i];
      if (round !== 0) invMixColumns(s);
    }
    return s;
  }

  function shiftRows(s) {
    let t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
  }

  function invShiftRows(s) {
    let t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
  }

  function mixColumns(s) {
    for (let c = 0; c < 4; c++) {
      const i = c * 4;
      const a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
      s[i] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
      s[i + 1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
      s[i + 2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
      s[i + 3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
    }
  }

  function invMixColumns(s) {
    for (let c = 0; c < 4; c++) {
      const i = c * 4;
      const a0 = s[i], a1 = s[i + 1], a2 = s[i + 2], a3 = s[i + 3];
      s[i] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
      s[i + 1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
      s[i + 2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
      s[i + 3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
    }
  }

  // AES-128-CBC with PKCS#7 padding added.
  function aesCbcEncrypt(key, iv, plain) {
    const rk = aesExpandKey(key);
    const padLen = 16 - (plain.length % 16);
    const padded = new Uint8Array(plain.length + padLen);
    padded.set(plain);
    padded.fill(padLen, plain.length);
    const out = new Uint8Array(padded.length);
    let prev = iv;
    for (let off = 0; off < padded.length; off += 16) {
      const block = padded.subarray(off, off + 16).slice();
      for (let i = 0; i < 16; i++) block[i] ^= prev[i];
      const enc = aesEncryptBlock(rk, block);
      out.set(enc, off);
      prev = enc;
    }
    return out;
  }

  // AES-128-CBC. Padding is NOT stripped -- ADEPT's device-key wrap and the
  // EPUB content path disagree about it, so the caller decides.
  function aesCbcDecryptRaw(key, iv, cipher) {
    if (cipher.length % 16 !== 0) throw new Error('AES-CBC: ciphertext is not a multiple of 16');
    const rk = aesExpandKey(key);
    const out = new Uint8Array(cipher.length);
    let prev = iv;
    for (let off = 0; off < cipher.length; off += 16) {
      const block = cipher.subarray(off, off + 16);
      const dec = aesDecryptBlock(rk, block);
      for (let i = 0; i < 16; i++) dec[i] ^= prev[i];
      out.set(dec, off);
      prev = block;
    }
    return out;
  }

  function stripPkcs7(bytes) {
    if (bytes.length === 0) return bytes;
    const pad = bytes[bytes.length - 1];
    if (pad < 1 || pad > 16 || pad > bytes.length) return bytes;
    return bytes.subarray(0, bytes.length - pad);
  }

  // ==========================================================================
  // RC2  (RFC 2268), CBC mode -- PKCS#12 cert bags
  // ==========================================================================
  //
  // Only decryption is needed: a PKCS#12 bundle arrives encrypted and is read
  // once. RC2-40 means an effective key length of 40 bits, which is what the
  // pbeWithSHA1And40BitRC2-CBC algorithm identifier asks for.

  const RC2_PITABLE = new Uint8Array([
    0xd9, 0x78, 0xf9, 0xc4, 0x19, 0xdd, 0xb5, 0xed, 0x28, 0xe9, 0xfd, 0x79, 0x4a, 0xa0, 0xd8, 0x9d,
    0xc6, 0x7e, 0x37, 0x83, 0x2b, 0x76, 0x53, 0x8e, 0x62, 0x4c, 0x64, 0x88, 0x44, 0x8b, 0xfb, 0xa2,
    0x17, 0x9a, 0x59, 0xf5, 0x87, 0xb3, 0x4f, 0x13, 0x61, 0x45, 0x6d, 0x8d, 0x09, 0x81, 0x7d, 0x32,
    0xbd, 0x8f, 0x40, 0xeb, 0x86, 0xb7, 0x7b, 0x0b, 0xf0, 0x95, 0x21, 0x22, 0x5c, 0x6b, 0x4e, 0x82,
    0x54, 0xd6, 0x65, 0x93, 0xce, 0x60, 0xb2, 0x1c, 0x73, 0x56, 0xc0, 0x14, 0xa7, 0x8c, 0xf1, 0xdc,
    0x12, 0x75, 0xca, 0x1f, 0x3b, 0xbe, 0xe4, 0xd1, 0x42, 0x3d, 0xd4, 0x30, 0xa3, 0x3c, 0xb6, 0x26,
    0x6f, 0xbf, 0x0e, 0xda, 0x46, 0x69, 0x07, 0x57, 0x27, 0xf2, 0x1d, 0x9b, 0xbc, 0x94, 0x43, 0x03,
    0xf8, 0x11, 0xc7, 0xf6, 0x90, 0xef, 0x3e, 0xe7, 0x06, 0xc3, 0xd5, 0x2f, 0xc8, 0x66, 0x1e, 0xd7,
    0x08, 0xe8, 0xea, 0xde, 0x80, 0x52, 0xee, 0xf7, 0x84, 0xaa, 0x72, 0xac, 0x35, 0x4d, 0x6a, 0x2a,
    0x96, 0x1a, 0xd2, 0x71, 0x5a, 0x15, 0x49, 0x74, 0x4b, 0x9f, 0xd0, 0x5e, 0x04, 0x18, 0xa4, 0xec,
    0xc2, 0xe0, 0x41, 0x6e, 0x0f, 0x51, 0xcb, 0xcc, 0x24, 0x91, 0xaf, 0x50, 0xa1, 0xf4, 0x70, 0x39,
    0x99, 0x7c, 0x3a, 0x85, 0x23, 0xb8, 0xb4, 0x7a, 0xfc, 0x02, 0x36, 0x5b, 0x25, 0x55, 0x97, 0x31,
    0x2d, 0x5d, 0xfa, 0x98, 0xe3, 0x8a, 0x92, 0xae, 0x05, 0xdf, 0x29, 0x10, 0x67, 0x6c, 0xba, 0xc9,
    0xd3, 0x00, 0xe6, 0xcf, 0xe1, 0x9e, 0xa8, 0x2c, 0x63, 0x16, 0x01, 0x3f, 0x58, 0xe2, 0x89, 0xa9,
    0x0d, 0x38, 0x34, 0x1b, 0xab, 0x33, 0xff, 0xb0, 0xbb, 0x48, 0x0c, 0x5f, 0xb9, 0xb1, 0xcd, 0x2e,
    0xc5, 0xf3, 0xdb, 0x47, 0xe5, 0xa5, 0x9c, 0x77, 0x0a, 0xa6, 0x20, 0x68, 0xfe, 0x7f, 0xc1, 0xad,
  ]);

  // RFC 2268 section 2: key expansion to 64 bytes, then reduction to `bits`.
  function rc2ExpandKey(key, bits) {
    const L = new Uint8Array(128);
    L.set(key);
    const T = key.length;
    for (let i = T; i < 128; i++) L[i] = RC2_PITABLE[(L[i - 1] + L[i - T]) & 0xff];
    const T8 = (bits + 7) >> 3;
    const TM = 0xff >> ((8 - (bits & 7)) & 7);
    L[128 - T8] = RC2_PITABLE[L[128 - T8] & TM];
    for (let i = 127 - T8; i >= 0; i--) L[i] = RC2_PITABLE[L[i + 1] ^ L[i + T8]];
    const K = new Uint16Array(64);
    for (let i = 0; i < 64; i++) K[i] = L[i * 2] | (L[i * 2 + 1] << 8);
    return K;
  }

  function rc2DecryptBlock(K, block) {
    const dv = new DataView(block.buffer, block.byteOffset, 8);
    let r0 = dv.getUint16(0, true), r1 = dv.getUint16(2, true);
    let r2 = dv.getUint16(4, true), r3 = dv.getUint16(6, true);
    const rol = (v, n) => ((v << n) | (v >>> (16 - n))) & 0xffff;
    const ror = (v, n) => ((v >>> n) | (v << (16 - n))) & 0xffff;

    let j = 63;
    // 16 mixing rounds with two mash rounds interleaved, run in reverse.
    for (let round = 0; round < 16; round++) {
      if (round === 5 || round === 11) {
        // r-mash (inverse): subtract the key selected by the neighbouring word.
        r3 = (r3 - K[r2 & 63]) & 0xffff;
        r2 = (r2 - K[r1 & 63]) & 0xffff;
        r1 = (r1 - K[r0 & 63]) & 0xffff;
        r0 = (r0 - K[r3 & 63]) & 0xffff;
      }
      r3 = ror(r3, 5);
      r3 = (r3 - K[j--] - (r2 & r1) - (~r2 & r0)) & 0xffff;
      r2 = ror(r2, 3);
      r2 = (r2 - K[j--] - (r1 & r0) - (~r1 & r3)) & 0xffff;
      r1 = ror(r1, 2);
      r1 = (r1 - K[j--] - (r0 & r3) - (~r0 & r2)) & 0xffff;
      r0 = ror(r0, 1);
      r0 = (r0 - K[j--] - (r3 & r2) - (~r3 & r1)) & 0xffff;
    }
    const out = new Uint8Array(8);
    const odv = new DataView(out.buffer);
    odv.setUint16(0, r0, true);
    odv.setUint16(2, r1, true);
    odv.setUint16(4, r2, true);
    odv.setUint16(6, r3, true);
    return out;
  }

  function rc2CbcDecrypt(key, iv, cipher, effectiveBits) {
    const K = rc2ExpandKey(key, effectiveBits || key.length * 8);
    const out = new Uint8Array(cipher.length);
    let prev = iv;
    for (let off = 0; off < cipher.length; off += 8) {
      const block = cipher.subarray(off, off + 8);
      const dec = rc2DecryptBlock(K, block.slice());
      for (let i = 0; i < 8; i++) dec[i] ^= prev[i];
      out.set(dec, off);
      prev = block;
    }
    return out;
  }

  // ==========================================================================
  // DES / 3DES  (FIPS 46-3), CBC mode -- PKCS#12 key bags
  // ==========================================================================
  //
  // Decryption only, same reason as RC2. A PKCS#12 written by OpenSSL protects
  // the shrouded key bag with pbeWithSHA1And3-KeyTripleDES-CBC.

  const DES_PC1 = [
    56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5,
    60, 52, 44, 36, 28, 20, 12, 4, 27, 19, 11, 3,
  ];
  const DES_PC2 = [
    13, 16, 10, 23, 0, 4, 2, 27, 14, 5, 20, 9, 22, 18, 11, 3, 25, 7, 15, 6, 26, 19, 12, 1,
    40, 51, 30, 36, 46, 54, 29, 39, 50, 44, 32, 47, 43, 48, 38, 55, 33, 52, 45, 41, 49, 35, 28, 31,
  ];
  const DES_SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1];
  const DES_IP = [
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7, 56, 48, 40, 32, 24, 16, 8, 0, 58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
  ];
  const DES_FP = [
    39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9, 49, 17, 57, 25, 32, 0, 40, 8, 48, 16, 56, 24,
  ];
  const DES_E = [
    31, 0, 1, 2, 3, 4, 3, 4, 5, 6, 7, 8, 7, 8, 9, 10, 11, 12, 11, 12, 13, 14, 15, 16,
    15, 16, 17, 18, 19, 20, 19, 20, 21, 22, 23, 24, 23, 24, 25, 26, 27, 28, 27, 28, 29, 30, 31, 0,
  ];
  const DES_P = [
    15, 6, 19, 20, 28, 11, 27, 16, 0, 14, 22, 25, 4, 17, 30, 9,
    1, 7, 23, 13, 31, 26, 2, 8, 18, 12, 29, 5, 21, 10, 3, 24,
  ];
  const DES_SBOX = [
    [14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13],
    [15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9],
    [10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12],
    [7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14],
    [2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3],
    [12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13],
    [4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12],
    [13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11],
  ];

  function bytesToBits(bytes) {
    const bits = new Uint8Array(bytes.length * 8);
    for (let i = 0; i < bytes.length; i++) {
      for (let b = 0; b < 8; b++) bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
    }
    return bits;
  }

  function bitsToBytes(bits) {
    const out = new Uint8Array(bits.length / 8);
    for (let i = 0; i < out.length; i++) {
      let v = 0;
      for (let b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
      out[i] = v;
    }
    return out;
  }

  function desSubkeys(keyBytes) {
    const key = bytesToBits(keyBytes);
    const cd = DES_PC1.map((i) => key[i]);
    let c = cd.slice(0, 28);
    let d = cd.slice(28);
    const subkeys = [];
    for (let round = 0; round < 16; round++) {
      const s = DES_SHIFTS[round];
      c = c.slice(s).concat(c.slice(0, s));
      d = d.slice(s).concat(d.slice(0, s));
      const cdNow = c.concat(d);
      subkeys.push(DES_PC2.map((i) => cdNow[i]));
    }
    return subkeys;
  }

  function desCrypt(subkeys, blockBytes, decrypt) {
    const input = bytesToBits(blockBytes);
    const ip = DES_IP.map((i) => input[i]);
    let l = ip.slice(0, 32);
    let r = ip.slice(32);
    for (let round = 0; round < 16; round++) {
      const k = subkeys[decrypt ? 15 - round : round];
      const expanded = DES_E.map((i) => r[i]);
      const xored = expanded.map((bit, i) => bit ^ k[i]);
      const sOut = new Uint8Array(32);
      for (let box = 0; box < 8; box++) {
        const o = box * 6;
        const row = (xored[o] << 1) | xored[o + 5];
        const col = (xored[o + 1] << 3) | (xored[o + 2] << 2) | (xored[o + 3] << 1) | xored[o + 4];
        const v = DES_SBOX[box][row * 16 + col];
        for (let b = 0; b < 4; b++) sOut[box * 4 + b] = (v >> (3 - b)) & 1;
      }
      const permuted = DES_P.map((i) => sOut[i]);
      const newR = l.map((bit, i) => bit ^ permuted[i]);
      l = r;
      r = newR;
    }
    const preOutput = r.concat(l);
    return bitsToBytes(new Uint8Array(DES_FP.map((i) => preOutput[i])));
  }

  // EDE: decrypt with K3, encrypt with K2, decrypt with K1.
  function tripleDesCbcDecrypt(key24, iv, cipher) {
    const k1 = desSubkeys(key24.subarray(0, 8));
    const k2 = desSubkeys(key24.subarray(8, 16));
    const k3 = desSubkeys(key24.subarray(16, 24));
    const out = new Uint8Array(cipher.length);
    let prev = iv;
    for (let off = 0; off < cipher.length; off += 8) {
      const block = cipher.subarray(off, off + 8);
      let t = desCrypt(k3, block, true);
      t = desCrypt(k2, t, false);
      t = desCrypt(k1, t, true);
      for (let i = 0; i < 8; i++) t[i] ^= prev[i];
      out.set(t, off);
      prev = block;
    }
    return out;
  }

  // ==========================================================================
  // ASN.1 DER
  // ==========================================================================

  // Minimal reader. Returns {tag, header, length, contents, end} at `offset`.
  function derRead(buf, offset) {
    const tag = buf[offset];
    let i = offset + 1;
    let len = buf[i++];
    if (len & 0x80) {
      const n = len & 0x7f;
      if (n === 0) throw new Error('DER: indefinite length not supported');
      len = 0;
      for (let k = 0; k < n; k++) len = len * 256 + buf[i++];
    }
    return { tag, headerEnd: i, length: len, contents: buf.subarray(i, i + len), end: i + len };
  }

  // Walk the immediate children of a constructed value.
  function derChildren(contents) {
    const out = [];
    let at = 0;
    while (at < contents.length) {
      const node = derRead(contents, at);
      out.push(node);
      at = node.end;
    }
    return out;
  }

  function derLengthBytes(len) {
    if (len < 0x80) return new Uint8Array([len]);
    const bytes = [];
    let v = len;
    while (v > 0) {
      bytes.unshift(v & 0xff);
      v >>>= 8;
    }
    return new Uint8Array([0x80 | bytes.length, ...bytes]);
  }

  function derEncode(tag, contents) {
    return concat([new Uint8Array([tag]), derLengthBytes(contents.length), contents]);
  }

  function derInteger(bigint) {
    let hex = bigint.toString(16);
    if (hex.length % 2) hex = '0' + hex;
    let bytes = new Uint8Array(hex.length / 2);
    for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
    // DER INTEGER is signed: a leading bit of 1 needs a 0x00 prefix.
    if (bytes[0] & 0x80) bytes = concat([new Uint8Array([0]), bytes]);
    return derEncode(0x02, bytes);
  }

  function derToBigInt(bytes) {
    let hex = '';
    for (const b of bytes) hex += b.toString(16).padStart(2, '0');
    return hex === '' ? 0n : BigInt('0x' + hex);
  }

  function derOid(bytes) {
    return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join('');
  }

  // ==========================================================================
  // RSA  (PKCS#1 v1.5)
  // ==========================================================================

  function bigIntToBytes(value, length) {
    let hex = value.toString(16);
    if (hex.length % 2) hex = '0' + hex;
    const raw = new Uint8Array(hex.length / 2);
    for (let i = 0; i < raw.length; i++) raw[i] = parseInt(hex.substr(i * 2, 2), 16);
    if (raw.length > length) throw new Error('RSA: value wider than the modulus');
    const out = new Uint8Array(length);
    out.set(raw, length - raw.length);
    return out;
  }

  function bytesToBigInt(bytes) {
    return derToBigInt(bytes);
  }

  // Right-to-left square-and-multiply. Not constant time: everything here runs
  // on the user's own machine against their own keys, and the alternative
  // (shipping a constant-time bignum) buys nothing against a local attacker who
  // already has the page.
  function modPow(base, exp, mod) {
    let result = 1n;
    base %= mod;
    while (exp > 0n) {
      if (exp & 1n) result = (result * base) % mod;
      exp >>= 1n;
      base = (base * base) % mod;
    }
    return result;
  }

  // Parse an X.509 certificate and return its RSA public key.
  //   Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
  //   tbsCertificate ::= SEQUENCE { [0] version, serial, sigAlg, issuer,
  //                                 validity, subject, subjectPublicKeyInfo, ... }
  function publicKeyFromCertificate(certDer) {
    const cert = derRead(certDer, 0);
    const tbs = derChildren(cert.contents)[0];
    const fields = derChildren(tbs.contents);
    // subjectPublicKeyInfo is the first SEQUENCE whose first child is the
    // rsaEncryption AlgorithmIdentifier; index it positionally, allowing for the
    // optional [0] version tag that v3 certificates carry.
    const hasVersion = fields[0].tag === 0xa0;
    const spki = fields[hasVersion ? 6 : 5];
    return publicKeyFromSpki(spki.contents, true);
  }

  // SubjectPublicKeyInfo ::= SEQUENCE { algorithm, subjectPublicKey BIT STRING }
  // The BIT STRING wraps RSAPublicKey ::= SEQUENCE { modulus, publicExponent }.
  function publicKeyFromSpki(contents, alreadyInner) {
    const children = alreadyInner ? derChildren(contents) : derChildren(derRead(contents, 0).contents);
    const bitString = children[1];
    // First contents byte of a BIT STRING is the count of unused trailing bits.
    const inner = bitString.contents.subarray(1);
    const rsaKey = derChildren(derRead(inner, 0).contents);
    return { n: derToBigInt(rsaKey[0].contents), e: derToBigInt(rsaKey[1].contents) };
  }

  // RSAES-PKCS1-v1_5 encryption (RFC 8017 section 7.2.1).
  // EM = 0x00 || 0x02 || PS (nonzero random, >= 8 bytes) || 0x00 || M
  function rsaEncryptPkcs1(pub, message) {
    const k = (pub.n.toString(16).length + 1) >> 1;
    if (message.length > k - 11) throw new Error('RSA: message too long for this key');
    const psLen = k - message.length - 3;
    const ps = new Uint8Array(psLen);
    // PS must contain no zero bytes; redraw any that come up zero.
    global.crypto.getRandomValues(ps);
    for (let i = 0; i < psLen; i++) {
      while (ps[i] === 0) ps[i] = randomBytes(1)[0];
    }
    const em = concat([new Uint8Array([0x00, 0x02]), ps, new Uint8Array([0x00]), message]);
    return bigIntToBytes(modPow(bytesToBigInt(em), pub.e, pub.n), k);
  }

  // Raw private operation, m^d mod n. ADEPT signs with type-1 padding it builds
  // itself and unwraps content keys from a raw block, so no padding is applied
  // here -- the caller owns the EM.
  function rsaPrivateRaw(priv, block) {
    const k = (priv.n.toString(16).length + 1) >> 1;
    return bigIntToBytes(modPow(bytesToBigInt(block), priv.d, priv.n), k);
  }

  // PKCS#1 v1.5 type-1 (signature) padding over an already-computed SHA-1.
  // EM = 0x00 || 0x01 || 0xFF... || 0x00 || hash
  // Adobe signs the bare 20-byte hash, with no DigestInfo wrapper -- which is
  // also why crypto.subtle's RSASSA-PKCS1-v1_5 could not have been used even in
  // a secure context: it always inserts DigestInfo.
  function rsaSignRawHash(priv, hash20) {
    const k = (priv.n.toString(16).length + 1) >> 1;
    const em = new Uint8Array(k);
    em.fill(0xff);
    em[0] = 0x00;
    em[1] = 0x01;
    em[k - 21] = 0x00;
    em.set(hash20, k - 20);
    return bigIntToBytes(modPow(bytesToBigInt(em), priv.d, priv.n), k);
  }

  // --- key generation -------------------------------------------------------

  function randomBigInt(bits) {
    const bytes = randomBytes(bits >> 3);
    bytes[0] |= 0xc0;                    // force the top two bits: full-width n
    bytes[bytes.length - 1] |= 1;        // odd
    return bytesToBigInt(bytes);
  }

  const SMALL_PRIMES = [
    3n, 5n, 7n, 11n, 13n, 17n, 19n, 23n, 29n, 31n, 37n, 41n, 43n, 47n, 53n, 59n, 61n, 67n, 71n,
    73n, 79n, 83n, 89n, 97n, 101n, 103n, 107n, 109n, 113n, 127n, 131n, 137n, 139n, 149n, 151n,
    157n, 163n, 167n, 173n, 179n, 181n, 191n, 193n, 197n, 199n, 211n,
  ];

  // Miller-Rabin. 24 random-base rounds puts the error probability below 2^-48,
  // far under the odds of a hardware fault during the same computation.
  function isProbablePrime(n, rounds) {
    if (n < 2n) return false;
    for (const p of SMALL_PRIMES) {
      if (n === p) return true;
      if (n % p === 0n) return false;
    }
    let d = n - 1n;
    let r = 0n;
    while ((d & 1n) === 0n) {
      d >>= 1n;
      r++;
    }
    const nBits = n.toString(2).length;
    for (let i = 0; i < (rounds || 24); i++) {
      let a;
      do {
        a = bytesToBigInt(randomBytes(Math.ceil(nBits / 8))) % n;
      } while (a < 2n);
      let x = modPow(a, d, n);
      if (x === 1n || x === n - 1n) continue;
      let composite = true;
      for (let j = 1n; j < r; j++) {
        x = (x * x) % n;
        if (x === n - 1n) {
          composite = false;
          break;
        }
      }
      if (composite) return false;
    }
    return true;
  }

  function generatePrime(bits, e) {
    for (;;) {
      let candidate = randomBigInt(bits);
      // Step by 2 through a window before redrawing: a fresh random candidate
      // costs an entropy call, and the density of primes makes this much faster.
      for (let step = 0; step < 4096; step++, candidate += 2n) {
        // p-1 must be coprime with e or d does not exist.
        if ((candidate - 1n) % e === 0n) continue;
        if (isProbablePrime(candidate)) return candidate;
      }
    }
  }

  function egcd(a, b) {
    if (b === 0n) return { g: a, x: 1n, y: 0n };
    const r = egcd(b, a % b);
    return { g: r.g, x: r.y, y: r.x - (a / b) * r.y };
  }

  function modInverse(a, m) {
    const r = egcd(((a % m) + m) % m, m);
    if (r.g !== 1n) throw new Error('RSA: no modular inverse');
    return ((r.x % m) + m) % m;
  }

  // RSA-1024 with e=65537, matching what the ADEPT client is expected to present.
  // Synchronous and CPU-bound: the caller should yield to the event loop before
  // calling so the page can paint a "this takes a moment" message first.
  function rsaGenerateKeyPair(bits) {
    const modulusBits = bits || 1024;
    const e = 65537n;
    let p, q, n;
    for (;;) {
      p = generatePrime(modulusBits / 2, e);
      q = generatePrime(modulusBits / 2, e);
      if (p === q) continue;
      n = p * q;
      // Reject a short modulus: the DER encoding and every downstream length
      // assumption depend on n being exactly `modulusBits` wide.
      if (n.toString(2).length === modulusBits) break;
    }
    if (p < q) {
      const t = p;
      p = q;
      q = t;
    }
    const phi = (p - 1n) * (q - 1n);
    const d = modInverse(e, phi);
    return {
      n, e, d, p, q,
      dP: d % (p - 1n),
      dQ: d % (q - 1n),
      qInv: modInverse(q, p),
    };
  }

  // --- key encoding ---------------------------------------------------------

  // rsaEncryption OID 1.2.840.113549.1.1.1, with the NULL parameters ADEPT expects.
  const RSA_ALG_ID = new Uint8Array([
    0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00,
  ]);

  // SubjectPublicKeyInfo ::= SEQUENCE { AlgorithmIdentifier, BIT STRING }
  function encodeSpki(key) {
    const rsaPublicKey = derEncode(0x30, concat([derInteger(key.n), derInteger(key.e)]));
    const bitString = derEncode(0x03, concat([new Uint8Array([0x00]), rsaPublicKey]));
    return derEncode(0x30, concat([RSA_ALG_ID, bitString]));
  }

  // PKCS#8 PrivateKeyInfo ::= SEQUENCE { version, AlgorithmIdentifier, OCTET STRING }
  // wrapping PKCS#1 RSAPrivateKey.
  function encodePkcs8(key) {
    const rsaPrivateKey = derEncode(0x30, concat([
      derInteger(0n), derInteger(key.n), derInteger(key.e), derInteger(key.d),
      derInteger(key.p), derInteger(key.q), derInteger(key.dP), derInteger(key.dQ), derInteger(key.qInv),
    ]));
    return derEncode(0x30, concat([
      derInteger(0n), RSA_ALG_ID, derEncode(0x04, rsaPrivateKey),
    ]));
  }

  // Read back a PKCS#8 private key (the credential stores keys in this form).
  function decodePkcs8(der) {
    const top = derChildren(derRead(der, 0).contents);
    const inner = derRead(top[2].contents, 0);
    const k = derChildren(inner.contents);
    return {
      n: derToBigInt(k[1].contents),
      e: derToBigInt(k[2].contents),
      d: derToBigInt(k[3].contents),
    };
  }

  // ==========================================================================
  // PKCS#12  (RFC 7292)
  // ==========================================================================

  // Appendix B.2 key derivation. `id` is 1 for a key, 2 for an IV, 3 for a MAC.
  // The password is the UTF-16BE encoding of the string, NUL-terminated.
  function pkcs12Kdf(password, salt, id, iterations, wantBytes) {
    const u = 20;  // SHA-1 output
    const v = 64;  // SHA-1 block

    const pwBytes = [];
    for (let i = 0; i < password.length; i++) {
      const c = password.charCodeAt(i);
      pwBytes.push((c >> 8) & 0xff, c & 0xff);
    }
    pwBytes.push(0, 0);
    const P = new Uint8Array(pwBytes);

    const D = new Uint8Array(v).fill(id);
    const buildBlock = (src) => {
      if (src.length === 0) return new Uint8Array(0);
      const len = v * Math.ceil(src.length / v);
      const out = new Uint8Array(len);
      for (let i = 0; i < len; i++) out[i] = src[i % src.length];
      return out;
    };
    const S = buildBlock(salt);
    const Pblk = buildBlock(P);
    const I = concat([S, Pblk]);

    const out = new Uint8Array(wantBytes);
    let produced = 0;
    let Icur = I;
    while (produced < wantBytes) {
      let A = sha1(concat([D, Icur]));
      for (let i = 1; i < iterations; i++) A = sha1(A);

      const take = Math.min(u, wantBytes - produced);
      out.set(A.subarray(0, take), produced);
      produced += take;
      if (produced >= wantBytes) break;

      // B = A repeated to v bytes; then each v-sized chunk of I becomes
      // (chunk + B + 1) mod 2^v, big-endian.
      const B = new Uint8Array(v);
      for (let i = 0; i < v; i++) B[i] = A[i % u];
      const next = new Uint8Array(Icur.length);
      for (let off = 0; off < Icur.length; off += v) {
        let carry = 1;
        for (let i = v - 1; i >= 0; i--) {
          const sum = Icur[off + i] + B[i] + carry;
          next[off + i] = sum & 0xff;
          carry = sum >> 8;
        }
      }
      Icur = next;
    }
    return out;
  }

  const OID_PBE_SHA1_RC2_40 = '2a864886f70d010c0106';   // 1.2.840.113549.1.12.1.6
  const OID_PBE_SHA1_3DES = '2a864886f70d010c0103';     // 1.2.840.113549.1.12.1.3
  const OID_DATA = '2a864886f70d010701';                // 1.2.840.113549.1.7.1
  const OID_ENCRYPTED_DATA = '2a864886f70d010706';      // 1.2.840.113549.1.7.6
  const OID_KEY_BAG = '2a864886f70d010c0a0101';         // keyBag
  const OID_PKCS8_SHROUDED_KEY_BAG = '2a864886f70d010c0a0102';
  const OID_CERT_BAG = '2a864886f70d010c0a0103';

  // Decrypt one PKCS#12 EncryptedData / EncryptedPrivateKeyInfo payload.
  // algId ::= SEQUENCE { OID, SEQUENCE { salt OCTET STRING, iterations INTEGER } }
  function pkcs12Decrypt(algIdNode, cipherBytes, password) {
    const alg = derChildren(algIdNode.contents);
    const oid = derOid(alg[0].contents);
    const params = derChildren(alg[1].contents);
    const salt = params[0].contents;
    const iterations = Number(derToBigInt(params[1].contents));

    if (oid === OID_PBE_SHA1_3DES) {
      const key = pkcs12Kdf(password, salt, 1, iterations, 24);
      const iv = pkcs12Kdf(password, salt, 2, iterations, 8);
      return stripPkcs7Block(tripleDesCbcDecrypt(key, iv, cipherBytes), 8);
    }
    if (oid === OID_PBE_SHA1_RC2_40) {
      const key = pkcs12Kdf(password, salt, 1, iterations, 5);
      const iv = pkcs12Kdf(password, salt, 2, iterations, 8);
      return stripPkcs7Block(rc2CbcDecrypt(key, iv, cipherBytes, 40), 8);
    }
    throw new Error('PKCS#12 uses an unsupported cipher (OID ' + oid + ')');
  }

  function stripPkcs7Block(bytes, blockSize) {
    if (bytes.length === 0) return bytes;
    const pad = bytes[bytes.length - 1];
    if (pad < 1 || pad > blockSize || pad > bytes.length) return bytes;
    return bytes.subarray(0, bytes.length - pad);
  }

  // Walk the SafeBags of one SafeContents and collect keys and certificates.
  function collectBags(safeContentsDer, password, found) {
    for (const bag of derChildren(derRead(safeContentsDer, 0).contents)) {
      const parts = derChildren(bag.contents);
      const bagOid = derOid(parts[0].contents);
      const bagValue = derRead(parts[1].contents, 0);  // [0] EXPLICIT

      if (bagOid === OID_PKCS8_SHROUDED_KEY_BAG) {
        // EncryptedPrivateKeyInfo ::= SEQUENCE { algId, encryptedData }
        const enc = derChildren(bagValue.contents);
        const pkcs8 = pkcs12Decrypt(enc[0], enc[1].contents, password);
        found.keys.push(pkcs8);
      } else if (bagOid === OID_KEY_BAG) {
        found.keys.push(bagValue.contents);
      } else if (bagOid === OID_CERT_BAG) {
        // CertBag ::= SEQUENCE { certId OID, certValue [0] EXPLICIT OCTET STRING }
        const cb = derChildren(bagValue.contents);
        const certValue = derRead(cb[1].contents, 0);
        found.certs.push(certValue.contents);
      }
    }
  }

  // Extract the private key (PKCS#8 DER) and its certificate (DER) from a
  // PKCS#12 bundle. Returns { key, cert } as Uint8Arrays.
  function pkcs12Extract(p12Bytes, password) {
    const pfx = derChildren(derRead(p12Bytes, 0).contents);
    // PFX ::= SEQUENCE { version, authSafe ContentInfo, macData OPTIONAL }
    const authSafe = derChildren(pfx[1].contents);
    if (derOid(authSafe[0].contents) !== OID_DATA) throw new Error('PKCS#12: authSafe is not plain data');
    const authSafeData = derRead(authSafe[1].contents, 0);  // [0] EXPLICIT OCTET STRING
    const outer = derRead(authSafeData.contents, 0);

    const found = { keys: [], certs: [] };
    for (const ci of derChildren(outer.contents)) {
      const parts = derChildren(ci.contents);
      const oid = derOid(parts[0].contents);
      if (oid === OID_DATA) {
        const octet = derRead(parts[1].contents, 0);
        collectBags(octet.contents, password, found);
      } else if (oid === OID_ENCRYPTED_DATA) {
        // EncryptedData ::= SEQUENCE { version, EncryptedContentInfo }
        const ed = derChildren(derRead(parts[1].contents, 0).contents);
        const eci = derChildren(ed[1].contents);
        // EncryptedContentInfo ::= SEQUENCE { contentType, algId, [0] IMPLICIT content }
        const cipher = eci[2].contents;
        const plain = pkcs12Decrypt(eci[1], cipher, password);
        collectBags(plain, password, found);
      }
    }

    if (found.keys.length === 0) throw new Error('PKCS#12: no private key found');
    if (found.certs.length === 0) throw new Error('PKCS#12: no certificate found');

    // Pair the key with the certificate whose public modulus matches it. wolfSSL
    // hit exactly this: a bundle carrying a chain hands back several certs, and
    // taking the first is a guess. Matching moduli makes the pairing certain.
    const priv = decodePkcs8(found.keys[0]);
    let cert = found.certs[0];
    for (const candidate of found.certs) {
      try {
        if (publicKeyFromCertificate(candidate).n === priv.n) {
          cert = candidate;
          break;
        }
      } catch (e) {
        // A cert we cannot parse simply is not the match; keep looking.
      }
    }
    return { key: found.keys[0], cert };
  }

  // ==========================================================================
  // exports
  // ==========================================================================

  global.LibbyCrypto = {
    // bytes
    utf8, concat, bytesToB64, b64ToBytes, bytesToHex, randomBytes,
    // digests
    sha1,
    // symmetric. RC2 and 3DES are exported (rather than kept private to the
    // PKCS#12 code that uses them) so they can be checked directly against
    // known-good implementations; see scripts/test_libby_crypto.mjs.
    aesCbcEncrypt, aesCbcDecryptRaw, stripPkcs7, rc2CbcDecrypt, tripleDesCbcDecrypt,
    // RSA
    rsaGenerateKeyPair, rsaEncryptPkcs1, rsaPrivateRaw, rsaSignRawHash,
    publicKeyFromCertificate, publicKeyFromSpki, encodeSpki, encodePkcs8, decodePkcs8,
    // PKCS#12
    pkcs12Extract,
    // DER (exposed for the ADEPT layer's certificate handling)
    derRead, derChildren, derEncode, derToBigInt,
  };
})(window);
