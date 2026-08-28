// Verifies src/network/html/js/libbyCrypto.js against Node's own crypto.
//
//   node scripts/test_libby_crypto.mjs
//
// That file implements SHA-1, AES-128-CBC, RC2-40, 3DES, RSA and PKCS#12 by
// hand, because the Libby page is served over plain http:// and therefore has
// no crypto.subtle. Hand-rolled crypto that is merely "probably right" is worse
// than none, so every primitive is checked here against a known-good
// implementation before it can reach a user's account.
//
// The PKCS#12 case builds a real bundle with openssl using PBE-SHA1-RC2-40 for
// the certificate bag and PBE-SHA1-3DES for the key bag -- the two ciphers an
// Adobe signing bundle actually uses -- so it covers RC2 even where this Node
// build has dropped rc2-40-cbc from its default provider.
import { readFileSync } from 'node:fs';
import crypto from 'node:crypto';
import vm from 'node:vm';

import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const src = readFileSync(join(here, '..', 'src', 'network', 'html', 'js', 'libbyCrypto.js'), 'utf8');

const sandbox = {
  window: {},
  crypto: { getRandomValues: (a) => { crypto.randomFillSync(a); return a; } },
  btoa: (s) => Buffer.from(s, 'binary').toString('base64'),
  atob: (s) => Buffer.from(s, 'base64').toString('binary'),
  TextEncoder,
  console,
};
sandbox.window.crypto = sandbox.crypto;
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(src, sandbox);
const C = sandbox.window.LibbyCrypto;

let pass = 0, fail = 0;
const check = (name, ok, detail) => {
  if (ok) { pass++; console.log(`  ok   ${name}`); }
  else { fail++; console.log(`  FAIL ${name}${detail ? ' -- ' + detail : ''}`); }
};
const hex = (u8) => Buffer.from(u8).toString('hex');

console.log('SHA-1');
for (const msg of ['', 'abc', 'The quick brown fox jumps over the lazy dog', 'a'.repeat(1000)]) {
  const mine = hex(C.sha1(new TextEncoder().encode(msg)));
  const theirs = crypto.createHash('sha1').update(msg).digest('hex');
  check(`sha1("${msg.slice(0, 20)}${msg.length > 20 ? '...' : ''}")`, mine === theirs, `${mine} != ${theirs}`);
}

console.log('AES-128-CBC');
for (let i = 0; i < 5; i++) {
  const key = crypto.randomBytes(16), iv = crypto.randomBytes(16);
  const pt = crypto.randomBytes(1 + Math.floor(Math.random() * 100));
  const c = crypto.createCipheriv('aes-128-cbc', key, iv);
  const theirs = Buffer.concat([c.update(pt), c.final()]);
  const mine = C.aesCbcEncrypt(new Uint8Array(key), new Uint8Array(iv), new Uint8Array(pt));
  check(`aes encrypt len=${pt.length}`, hex(mine) === theirs.toString('hex'), `${hex(mine)} != ${theirs.toString('hex')}`);

  const back = C.stripPkcs7(C.aesCbcDecryptRaw(new Uint8Array(key), new Uint8Array(iv), mine));
  check(`aes roundtrip len=${pt.length}`, hex(back) === pt.toString('hex'));
}

console.log('3DES-CBC decrypt');
for (let i = 0; i < 3; i++) {
  const key = crypto.randomBytes(24), iv = crypto.randomBytes(8);
  const pt = crypto.randomBytes(16);
  const c = crypto.createCipheriv('des-ede3-cbc', key, iv);
  c.setAutoPadding(false);
  const ct = Buffer.concat([c.update(pt), c.final()]);
  const mine = C.tripleDesCbcDecrypt
    ? C.tripleDesCbcDecrypt(new Uint8Array(key), new Uint8Array(iv), new Uint8Array(ct))
    : null;
  if (mine === null) { console.log('  skip 3DES (not exported)'); break; }
  check(`3des len=${pt.length}`, hex(mine) === pt.toString('hex'), `${hex(mine)} != ${pt.toString('hex')}`);
}

console.log('RC2-40-CBC decrypt');
for (let i = 0; i < 3; i++) {
  const key = crypto.randomBytes(5), iv = crypto.randomBytes(8);
  const pt = crypto.randomBytes(16);
  let ct;
  try {
    const c = crypto.createCipheriv('rc2-40-cbc', key, iv);
    c.setAutoPadding(false);
    ct = Buffer.concat([c.update(pt), c.final()]);
  } catch (e) { console.log('  skip RC2 (node build lacks rc2-40-cbc: ' + e.message + ')'); break; }
  const mine = C.rc2CbcDecrypt
    ? C.rc2CbcDecrypt(new Uint8Array(key), new Uint8Array(iv), new Uint8Array(ct), 40)
    : null;
  if (mine === null) { console.log('  skip RC2 (not exported)'); break; }
  check(`rc2 len=${pt.length}`, hex(mine) === pt.toString('hex'), `${hex(mine)} != ${pt.toString('hex')}`);
}

console.log('RSA keygen + encode + sign/verify');
const t0 = Date.now();
const kp = C.rsaGenerateKeyPair(1024);
console.log(`  (keygen took ${Date.now() - t0} ms)`);
check('modulus is 1024 bits', kp.n.toString(2).length === 1024, `${kp.n.toString(2).length}`);

const spki = C.encodeSpki(kp);
const pkcs8 = C.encodePkcs8(kp);
let nodePub, nodePriv;
try {
  nodePub = crypto.createPublicKey({ key: Buffer.from(spki), format: 'der', type: 'spki' });
  check('SPKI parses in node', true);
} catch (e) { check('SPKI parses in node', false, e.message); }
try {
  nodePriv = crypto.createPrivateKey({ key: Buffer.from(pkcs8), format: 'der', type: 'pkcs8' });
  check('PKCS#8 parses in node', true);
} catch (e) { check('PKCS#8 parses in node', false, e.message); }

if (nodePub && nodePriv) {
  // Our PKCS#1 v1.5 encrypt -> node decrypts
  const msg = Buffer.from('hello adept');
  const ct = C.rsaEncryptPkcs1({ n: kp.n, e: kp.e }, new Uint8Array(msg));
  try {
    const back = crypto.privateDecrypt(
      { key: nodePriv, padding: crypto.constants.RSA_PKCS1_PADDING }, Buffer.from(ct));
    check('rsaEncryptPkcs1 -> node privateDecrypt', back.toString() === msg.toString(), back.toString());
  } catch (e) { check('rsaEncryptPkcs1 -> node privateDecrypt', false, e.message); }

  // Our raw type-1 signature over a bare SHA-1 -> verify by raw public op
  const hash = C.sha1(new TextEncoder().encode('sign me'));
  const sig = C.rsaSignRawHash(kp, hash);
  const recovered = crypto.publicEncrypt(
    { key: nodePub, padding: crypto.constants.RSA_NO_PADDING }, Buffer.from(sig));
  const em = new Uint8Array(recovered);
  const okShape = em[0] === 0x00 && em[1] === 0x01 && em[em.length - 21] === 0x00;
  const okHash = hex(em.subarray(em.length - 20)) === hex(hash);
  check('rsaSignRawHash produces valid type-1 EM', okShape && okHash, hex(em.subarray(0, 8)));
}

console.log('X.509 public key extraction');
{
  const { publicKey, privateKey } = crypto.generateKeyPairSync('rsa', { modulusLength: 1024 });
  const cert = null; // build a self-signed cert via X509 if available
  try {
    // Node >= 15.6 can't self-sign easily; instead verify SPKI path.
    const spkiDer = publicKey.export({ format: 'der', type: 'spki' });
    const parsed = C.publicKeyFromSpki(new Uint8Array(spkiDer), false);
    const nodeJwk = publicKey.export({ format: 'jwk' });
    const nMine = parsed.n.toString(16);
    const nTheirs = Buffer.from(nodeJwk.n, 'base64url').toString('hex').replace(/^0+/, '');
    check('publicKeyFromSpki modulus', nMine === nTheirs, `${nMine.slice(0,16)} != ${nTheirs.slice(0,16)}`);
    check('publicKeyFromSpki exponent', parsed.e === 65537n, parsed.e.toString());
  } catch (e) { check('publicKeyFromSpki', false, e.message); }
}

console.log('PKCS#12 extract');
{
  // Build a real PKCS#12 with openssl if available.
  const { execSync } = await import('node:child_process');
  const os = await import('node:os');
  const path = await import('node:path');
  const fs = await import('node:fs');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'p12-'));
  try {
    execSync(`openssl req -x509 -newkey rsa:1024 -keyout ${dir}/k.pem -out ${dir}/c.pem -days 1 -nodes -subj "/CN=test" 2>/dev/null`);
    // -certpbe/-keypbe force the two legacy ciphers a real ADEPT bundle uses.
    execSync(`openssl pkcs12 -export -inkey ${dir}/k.pem -in ${dir}/c.pem -out ${dir}/b.p12 -passout pass:secret -certpbe PBE-SHA1-RC2-40 -keypbe PBE-SHA1-3DES -macalg sha1 -legacy 2>/dev/null || openssl pkcs12 -export -inkey ${dir}/k.pem -in ${dir}/c.pem -out ${dir}/b.p12 -passout pass:secret -certpbe PBE-SHA1-RC2-40 -keypbe PBE-SHA1-3DES -macalg sha1 2>/dev/null`);
    const p12 = new Uint8Array(fs.readFileSync(`${dir}/b.p12`));
    const out = C.pkcs12Extract(p12, 'secret');
    check('pkcs12 returned a key', out.key && out.key.length > 0);
    check('pkcs12 returned a cert', out.cert && out.cert.length > 0);
    // The extracted key must parse and match the extracted cert's public key.
    const priv = C.decodePkcs8(out.key);
    const certPub = C.publicKeyFromCertificate(out.cert);
    check('pkcs12 key matches cert', priv.n === certPub.n,
      `${priv.n.toString(16).slice(0,16)} != ${certPub.n.toString(16).slice(0,16)}`);
    // And node must accept the PKCS#8 we handed back.
    crypto.createPrivateKey({ key: Buffer.from(out.key), format: 'der', type: 'pkcs8' });
    check('pkcs12 key is valid PKCS#8', true);
  } catch (e) {
    check('pkcs12 extract', false, e.message);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
