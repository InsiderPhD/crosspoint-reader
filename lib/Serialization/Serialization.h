#pragma once
#include <HalStorage.h>

#include <iostream>
#include <limits>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

// Length-checked POD write: returns false if the full object was not written.
template <typename T>
static bool tryWritePod(FsFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

// Length-checked POD read: returns false on short/failed read (corrupt/truncated file).
template <typename T>
static bool tryReadPod(FsFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

// Length-checked string write: returns false if length prefix or payload was truncated.
static bool tryWriteString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(file, len) && (len == 0 || file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

static void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}

// Length-checked string read: rejects implausible lengths and short reads (corrupt file).
static bool tryReadString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (!tryReadPod(file, len)) {
    return false;
  }
  if (static_cast<size_t>(len) > s.max_size() || len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  s.resize(len);
  const int readLen = static_cast<int>(len);
  return len == 0 || file.read(&s[0], readLen) == readLen;
}
}  // namespace serialization
