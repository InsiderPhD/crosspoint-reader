#include "ObfuscationUtils.h"

#include <Logging.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstring>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;

// Simple lazy init — no thread-safety concern on single-core ESP32-C3.
const uint8_t* getHwKey() {
  static uint8_t key[HW_KEY_LEN] = {};
  static bool initialized = false;
  if (!initialized) {
    esp_efuse_mac_get_default(key);
    initialized = true;
  }
  return key;
}

// CrossInk's "validated" payload: XOR-obfuscated "CPV1" + FNV-1a(key, plaintext)
// as uint32 LE + plaintext. Same hardware key and base64 wrapping as the legacy
// form, so wifi.json / opds.json / koreader.json written by CrossInk decode here.
// This firmware keeps WRITING the legacy form, the one upstream CrossPoint and
// CrossInk both read.
constexpr char VALIDATED_PREFIX[] = {'C', 'P', 'V', '1'};
constexpr size_t VALIDATED_PREFIX_LEN = sizeof(VALIDATED_PREFIX);
constexpr size_t VALIDATED_CHECKSUM_LEN = 4;

uint32_t validatedChecksum(const char* plaintext, size_t len) {
  uint32_t hash = 2166136261UL;
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < HW_KEY_LEN; i++) hash = (hash ^ key[i]) * 16777619UL;
  for (size_t i = 0; i < len; i++) hash = (hash ^ static_cast<uint8_t>(plaintext[i])) * 16777619UL;
  return hash;
}

// Strips a CrossInk validated header in place when its checksum matches. A
// mismatch leaves the string alone: it is then a legacy payload whose
// plaintext merely happens to start with those bytes.
void unwrapValidated(std::string& data) {
  constexpr size_t headerLen = VALIDATED_PREFIX_LEN + VALIDATED_CHECKSUM_LEN;
  if (data.size() < headerLen || memcmp(data.data(), VALIDATED_PREFIX, VALIDATED_PREFIX_LEN) != 0) return;
  const auto* c = reinterpret_cast<const uint8_t*>(data.data() + VALIDATED_PREFIX_LEN);
  const uint32_t expected = static_cast<uint32_t>(c[0]) | (static_cast<uint32_t>(c[1]) << 8) |
                            (static_cast<uint32_t>(c[2]) << 16) | (static_cast<uint32_t>(c[3]) << 24);
  if (validatedChecksum(data.data() + headerLen, data.size() - headerLen) != expected) return;
  data.erase(0, headerLen);
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

void xorTransform(std::string& data, const uint8_t* key, size_t keyLen) {
  if (keyLen == 0 || key == nullptr) return;
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % keyLen];
  }
}

String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return "";
  std::string temp = plaintext;
  xorTransform(temp);
  return base64::encode(reinterpret_cast<const uint8_t*>(temp.data()), temp.size());
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return "";
  }
  if (ok) *ok = true;
  size_t encodedLen = strlen(encoded);
  // First call: get required output buffer size
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  result.resize(decodedLen);
  xorTransform(result);
  unwrapValidated(result);
  return result;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    String encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  {
    // A CrossInk-written (validated) payload decodes to its plaintext.
    const std::string plain = "swap me";
    std::string payload(VALIDATED_PREFIX, VALIDATED_PREFIX_LEN);
    const uint32_t sum = validatedChecksum(plain.data(), plain.size());
    for (int shift = 0; shift < 32; shift += 8) payload.push_back(static_cast<char>((sum >> shift) & 0xFF));
    payload += plain;
    xorTransform(payload);
    const String encoded = base64::encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    if (deobfuscateFromBase64(encoded.c_str()) != plain) {
      LOG_ERR("OBF", "FAIL: validated (CrossInk) payload did not decode");
      allPassed = false;
    }
  }
  // Verify obfuscated form differs from plaintext
  String enc = obfuscateToBase64("test123");
  if (enc == "test123") {
    LOG_ERR("OBF", "FAIL: obfuscated output identical to plaintext");
    allPassed = false;
  }
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
