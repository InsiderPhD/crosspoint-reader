#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <memory>

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  FsFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

constexpr uint32_t ZIP_CENTRAL_DIR_SIGNATURE = 0x02014b50;
constexpr size_t ZIP_CENTRAL_DIR_HEADER_SIZE = 46;
constexpr size_t ZIP_CENTRAL_DIR_CHUNK_SIZE = 1024;
constexpr uint16_t ZIP_MAX_STORED_NAME_LEN = 255;  // callers pass a 256-byte name buffer

struct CentralDirEntry {
  uint32_t entryStart = 0;  // file offset of this entry's signature
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localHeaderOffset = 0;
  uint16_t method = 0;
  uint16_t nameLen = 0;
  bool nameStored = false;  // false when the name was too long and skipped
};

// Sequential reader for the ZIP central directory. The previous loops made
// ~13 mutex-locked SD calls per entry (nine 2-4 byte reads plus seeks), and
// every uncached chapter/image/stylesheet lookup scans this directory, so
// large EPUBs paid that thousands of times. This reads 1 KiB chunks and parses
// the 46-byte headers from RAM. The chunk is heap-allocated (1 KiB is over the
// 256-byte stack-local budget) once per scan and freed when the scan ends; if
// the allocation fails the cursor degrades to one header read, one name read
// and one seek per entry. Multi-byte fields are copied out with memcpy because
// entries land at arbitrary (unaligned) offsets inside the chunk.
class CentralDirCursor final {
 public:
  CentralDirCursor(HalFile& file, const uint32_t offset)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(ZIP_CENTRAL_DIR_CHUNK_SIZE)) {
    if (!buf) LOG_ERR("ZIP", "Central directory buffer alloc failed; using unbuffered scan");
    restart(offset);
  }
  CentralDirCursor(const CentralDirCursor&) = delete;
  CentralDirCursor& operator=(const CentralDirCursor&) = delete;

  void restart(const uint32_t offset) {
    ok = file.seek(offset);
    filePos = offset;
    fill = 0;
    off = 0;
  }

  // File offset of the next unread entry.
  uint32_t position() const { return filePos - static_cast<uint32_t>(fill - off); }

  // Reads the next entry. `name` must hold ZIP_MAX_STORED_NAME_LEN + 1 bytes.
  // Returns false at the end of the directory or on a read/seek failure.
  bool next(CentralDirEntry& entry, char* name) {
    if (!ok) return false;
    entry.entryStart = position();
    uint8_t direct[ZIP_CENTRAL_DIR_HEADER_SIZE];
    const uint8_t* header = direct;
    if (buf) {
      if (!ensure(ZIP_CENTRAL_DIR_HEADER_SIZE)) return false;
      header = buf.get() + off;
    } else if (!readDirect(direct, ZIP_CENTRAL_DIR_HEADER_SIZE)) {
      return false;
    }

    uint32_t signature = 0;
    uint16_t extraLen = 0;
    uint16_t commentLen = 0;
    memcpy(&signature, header, 4);
    if (signature != ZIP_CENTRAL_DIR_SIGNATURE) return false;
    memcpy(&entry.method, header + 10, 2);
    memcpy(&entry.compressedSize, header + 20, 4);
    memcpy(&entry.uncompressedSize, header + 24, 4);
    memcpy(&entry.nameLen, header + 28, 2);
    memcpy(&extraLen, header + 30, 2);
    memcpy(&commentLen, header + 32, 2);
    memcpy(&entry.localHeaderOffset, header + 42, 4);
    if (buf) off += ZIP_CENTRAL_DIR_HEADER_SIZE;

    entry.nameStored = entry.nameLen <= ZIP_MAX_STORED_NAME_LEN;
    if (entry.nameStored) {
      if (buf) {
        if (!ensure(entry.nameLen)) return false;
        memcpy(name, buf.get() + off, entry.nameLen);
        off += entry.nameLen;
      } else if (!readDirect(reinterpret_cast<uint8_t*>(name), entry.nameLen)) {
        return false;
      }
      name[entry.nameLen] = '\0';
    } else if (!skip(entry.nameLen)) {
      return false;
    }
    // Like the old scan, report this entry even if its trailing fields cannot
    // be skipped; the failed seek ends the scan at the next call.
    skip(static_cast<size_t>(extraLen) + commentLen);
    return true;
  }

 private:
  // Makes `count` (<= chunk size) bytes available at buf[off].
  bool ensure(const size_t count) {
    if (fill - off >= count) return true;
    memmove(buf.get(), buf.get() + off, fill - off);
    fill -= off;
    off = 0;
    while (fill < count) {
      const int got = file.read(buf.get() + fill, ZIP_CENTRAL_DIR_CHUNK_SIZE - fill);
      if (got <= 0) return false;
      fill += static_cast<size_t>(got);
      filePos += static_cast<uint32_t>(got);
    }
    return true;
  }

  bool readDirect(uint8_t* out, const size_t count) {
    if (count == 0) return true;
    const int got = file.read(out, count);
    if (got < 0 || static_cast<size_t>(got) != count) return false;
    filePos += static_cast<uint32_t>(count);
    return true;
  }

  bool skip(const size_t count) {
    const size_t buffered = fill - off;
    if (count <= buffered) {
      off += count;
      return true;
    }
    const size_t rest = count - buffered;
    fill = 0;
    off = 0;
    if (!file.seekCur(static_cast<int64_t>(rest))) {
      ok = false;
      return false;
    }
    filePos += static_cast<uint32_t>(rest);
    return true;
  }

  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  uint32_t filePos = 0;  // file offset just past the bytes read so far
  size_t fill = 0;
  size_t off = 0;
  bool ok = true;
};

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  // HalFile::read() returns a negative int on error. Treat it as end-of-stream
  // rather than letting the negative-to-size_t conversion underflow fileRemaining
  // and report a huge bytesRead, which would have the inflate library read past
  // the end of readBuf.
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return -1;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  if (bytesRead == 0) return -1;

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}
}  // namespace

// Single non-template body for enumerateFilePaths(): the header's template is
// now a thin stack-only adapter, so flash holds one copy of this scan however
// many distinct callback types instantiate it.
bool ZipFile::enumerateFilePathsImpl(void* context, void (*callback)(void*, std::string_view)) {
  if (!fileStatSlimCache.empty()) {
    for (const auto& entry : fileStatSlimCache) callback(context, std::string_view{entry.first});
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;
  while (cursor.next(entry, itemName)) {
    if (entry.nameStored) callback(context, std::string_view{itemName, entry.nameLen});
  }
  return true;
}

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;
  while (cursor.next(entry, itemName)) {
    if (!entry.nameStored) continue;  // oversized names cannot be looked up
    FileStatSlim fileStat = {};
    fileStat.method = entry.method;
    fileStat.compressedSize = entry.compressedSize;
    fileStat.uncompressedSize = entry.uncompressedSize;
    fileStat.localHeaderOffset = entry.localHeaderOffset;
    fileStatSlimCache.emplace(itemName, fileStat);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  const uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, startPos);
  CentralDirEntry entry;

  while (true) {
    if (!cursor.next(entry, itemName)) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        cursor.restart(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entry.entryStart >= startPos) {
      break;
    }

    if (entry.nameStored && strcmp(itemName, filename) == 0) {
      fileStat->method = entry.method;
      fileStat->compressedSize = entry.compressedSize;
      fileStat->uncompressedSize = entry.uncompressedSize;
      fileStat->localHeaderOffset = entry.localHeaderOffset;
      // Found it! Update cursor to next entry
      lastCentralDirPos = cursor.position();
      lastCentralDirPosValid = true;
      found = true;
      break;
    }
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const int read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  if (file.read(buffer, scanRange) != scanRange) {
    LOG_ERR("ZIP", "Failed to read EOCD scan window");
    free(buffer);
    return false;
  }

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;

  while (cursor.next(entry, itemName)) {
    if (!entry.nameStored) continue;

    const uint16_t nameLen = entry.nameLen;
    const uint64_t hash = fnvHash64(itemName, nameLen);
    const SizeTarget key = {hash, nameLen, 0};

    auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    while (it != targets.end() && it->hash == hash && it->len == nameLen) {
      if (it->index < sizes.size()) {
        sizes[it->index] = entry.uncompressedSize;
        matched++;
      }
      ++it;
    }

    if (matched >= targetCount) {
      break;
    }
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const int dataRead = file.read(data, inflatedDataSize);

    if (dataRead < 0 || static_cast<uint32_t>(dataRead) != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %u got %d", inflatedDataSize, dataRead);
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const int dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead < 0 || static_cast<uint32_t>(dataRead) != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %u got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const int result = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      // A negative error result must not reach the size_t arithmetic below: it would
      // underflow `remaining` and hand out.write() a buffer length far past `buffer`.
      if (result <= 0) {
        LOG_ERR("ZIP", "Could not read more bytes (%d)", result);
        free(buffer);
        return false;
      }
      const size_t dataRead = static_cast<size_t>(result);

      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    if (!ctx.reader.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate reader");
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStatus::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // ctx.reader destructor frees the ring buffer
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
