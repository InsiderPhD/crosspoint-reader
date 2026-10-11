#pragma once
#include <HalStorage.h>
#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Print-typed writers for the section-cache page serializers, which write through a
// BufferedPrint instead of straight into the file (see Section::onPageComplete). They emit
// exactly the bytes their FsFile counterparts in Serialization.h emit. Kept in a separate
// header so the many Serialization.h users do not grow a second overload set; a HalFile
// argument still selects the FsFile overloads there (exact match beats derived-to-base).
//
// Never serialise size_t or a pointer through these: the simulator is a 64-bit host and the
// cache must stay byte-identical with the device's.
namespace serialization {

template <typename T>
inline void writePod(Print& out, const T& value) {
  out.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

// Length-checked POD write: returns false if the full object was not accepted.
template <typename T>
inline bool tryWritePod(Print& out, const T& value) {
  return out.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

inline void writeString(Print& out, const std::string& s) {
  const uint32_t len = s.size();
  writePod(out, len);
  out.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Length-checked string write: returns false if the length prefix or payload was truncated.
inline bool tryWriteString(Print& out, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(out, len) && (len == 0 || out.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

// A HalFile seen as a block-writing Print.
//
// HalFile IS a Print, but on the device it only overrides the single-byte write(uint8_t);
// its block write takes `const void*` and does not override Print::write(const uint8_t*,
// size_t). Calling that through a Print& therefore lands in Arduino's default, which loops
// over write(uint8_t): one storage-mutex take and one SdFat call PER BYTE. Anything that
// hands a file to Print-typed code for bulk output must go through this adapter, which
// forwards the whole block in one locked call. Borrows the file; it must outlive this.
class HalFilePrint final : public Print {
 public:
  explicit HalFilePrint(HalFile& file) : file_(file) {}
  HalFilePrint(const HalFilePrint&) = delete;
  HalFilePrint& operator=(const HalFilePrint&) = delete;

  size_t write(const uint8_t b) override { return file_.write(static_cast<const void*>(&b), 1); }
  size_t write(const uint8_t* data, const size_t n) override { return file_.write(static_cast<const void*>(data), n); }

 private:
  HalFile& file_;
};

}  // namespace serialization
