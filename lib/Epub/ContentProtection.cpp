// SD/HAL binding for the content-protection read path.
//
// The ContentProtection SDK lib is storage-agnostic (it works against a
// ByteSource). This file is the firmware-side glue that backs that seam with
// the device's SD storage: a HalStorage-backed ByteSource, the credential
// lookup, and the openProtectedBook() entry point the reader calls. It lives in
// the firmware — not the SDK lib — so the portable lib carries no HAL dependency.

#include <ByteSource.h>
#include <ContentProtection.h>
#include <Credential.h>
#include <HalStorage.h>
#include <InflateAllocator.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <ProtectedBook.h>
#include <WolfsslCrypto.h>
#include <Zip.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>

#include <ctime>

#include "../../src/util/TimeUtils.h"

namespace freeink {
namespace content {

namespace {

// The access credential is provisioned off-device and dropped here.
// Generic path — the reader carries no scheme name.
constexpr const char* kCredentialPath = "/.crosspoint/content.key";

// miniz takes its entire inflate state in ONE ~44KB allocation (the 32KB
// dictionary the deflate format mandates, plus Huffman tables). That single
// request is what fails here: opening a 143MB protected comic, free heap was
// 61,720 bytes while the largest free block was 21,492 -- fragmentation, not
// exhaustion, and no amount of freeing fixes it.
//
// So serve it from the region an InflateScratchLease is already lending (the
// idle framebuffer, contiguous since boot). uzlib and miniz never inflate at
// the same time, so they share that one region rather than each claiming the
// framebuffer and overlapping. With no lease outstanding this is plain malloc,
// which is what host builds and PSRAM boards get.
void* contentInflateAlloc(void*, size_t items, size_t size) {
  const size_t total = items * size;
  if (uint8_t* lent = borrowInflateScratch(total)) return lent;
  return malloc(total);
}

void contentInflateFree(void*, void* address) {
  if (address == nullptr) return;
  if (returnInflateScratch(static_cast<const uint8_t*>(address))) return;
  free(address);
}

// Installed once, lazily: the allocator itself is inert until a lease exists.
void ensureInflateAllocator() {
  static bool installed = false;
  if (installed) return;
  setInflateAllocator(&contentInflateAlloc, &contentInflateFree, nullptr);
  installed = true;
}

// One shared crypto backend for the whole read path.
WolfsslCrypto& crypto() {
  static WolfsslCrypto instance;
  return instance;
}

// ByteSource over an SD file (read-only). One open handle per instance.
class SdByteSource : public ByteSource {
 public:
  explicit SdByteSource(std::string path) : path_(std::move(path)) {}
  bool open() {
    file_ = Storage.open(path_.c_str(), O_RDONLY);
    return file_ && file_.isOpen();
  }
  // Open once, then reuse across reads — decrypting a book faults many entries.
  bool ensureOpen() { return (file_ && file_.isOpen()) || open(); }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_ || !file_.seek64(offset)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return file_ ? file_.fileSize64() : 0; }

 private:
  std::string path_;
  mutable HalFile file_;
};

// Adapts an opened ProtectedBook to the reader-facing access interface.
class ProtectedBookDecryptor : public ContentDecryptor {
 public:
  ProtectedBookDecryptor(std::string epubPath, std::unique_ptr<ProtectedBook> book)
      : source_(std::move(epubPath)), book_(std::move(book)) {}

  bool isEncrypted(const std::string& itemPath) const override { return book_->isEncrypted(itemPath); }

  size_t decryptedSize(const std::string& itemPath) const override { return book_->decryptedSize(itemPath); }

  bool decryptToSink(const std::string& itemPath, ContentChunkSink sink, void* context) override {
    // Reuse one open SD handle for the whole reader session rather than
    // reconstructing and reopening it per encrypted entry.
    if (!source_.ensureOpen()) return false;
    return book_->decryptEntryToSink(source_, crypto(), itemPath, sink, context);
  }

 private:
  SdByteSource source_;
  std::unique_ptr<ProtectedBook> book_;
};

}  // namespace

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err) {
  err.clear();
  if (!Storage.exists(epubPath.c_str())) return nullptr;

  // Cheap gate first. ZipScan below indexes the whole central directory into a
  // vector (~24 bytes an entry) on top of a 4KB EOCD window, and every book the
  // reader opens would pay that just to learn it is not protected. ZipFile
  // streams the directory looking for one name with no per-entry heap at all,
  // so an ordinary book now pays a directory walk and nothing else — worth it
  // on a device where a section rebuild is already scraping the largest free
  // block.
  {
    size_t encryptionXmlSize = 0;
    ZipFile probe(epubPath);
    if (!probe.getInflatedFileSize("META-INF/encryption.xml", &encryptionXmlSize)) return nullptr;
  }

  ensureInflateAllocator();

  SdByteSource source(epubPath);
  if (!source.open()) return nullptr;

  // Index the container once and transfer that index into ProtectedBook, so a
  // protected book scans only the one extra time.
  ZipScan scan;
  if (!scan.open(source) || !scan.find("META-INF/encryption.xml")) return nullptr;

  // A book carrying encryption.xml may only obfuscate its embedded fonts
  // (not content-protected). The SDK demands the credential only after parsing
  // the manifest and finding genuinely encrypted entries.
  SdByteSource credSource(kCredentialPath);
  Credential credential;
  const bool haveCredential = credSource.open() && parseCredential(credSource, &credential);

  auto book = makeUniqueNoThrow<ProtectedBook>();
  if (!book) {
    err = "out of memory";
    return nullptr;
  }
  // Prefer an out-of-band rights document delivered as a sidecar next to the
  // EPUB ("<book>.epub.rights"), so the EPUB on disk stays byte-identical to
  // what the server sent. Falls back to a rights.xml injected into the zip.
  std::string rightsOverride;
  {
    // A real rights document is a few KB; 64KB is a generous ceiling. The
    // largest-block check keeps the resize below from aborting on OOM (string
    // growth is a bare allocation under -fno-exceptions).
    constexpr uint64_t kMaxRightsSize = 64 * 1024;
    SdByteSource rightsSource(epubPath + ".rights");
    if (rightsSource.open()) {
      const uint64_t rsize = rightsSource.size();
      if (rsize > 0 && rsize <= kMaxRightsSize &&
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > static_cast<size_t>(rsize) + 8 * 1024) {
        rightsOverride.resize(static_cast<size_t>(rsize));
        const int32_t rn = rightsSource.readAt(0, rightsOverride.data(), static_cast<uint32_t>(rsize));
        if (rn <= 0)
          rightsOverride.clear();
        else
          rightsOverride.resize(static_cast<size_t>(rn));
      }
    }
  }
  if (!book->openFromScan(source, crypto(), credential, std::move(scan), rightsOverride)) {
    // Several failures in here are really allocation failures wearing another
    // name (miniz's inflate state, the manifest scan buffers). Record the heap
    // shape alongside the reason so the two are distinguishable from a log.
    LOG_ERR("CPROT", "openFromScan failed: %s (heap %u, max block %u, credential %s)", book->lastError().c_str(),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            haveCredential ? "present" : "missing");
    err = haveCredential ? ("cannot open protected content: " + book->lastError())
                         : "no content access key on this device";
    return nullptr;
  }
  // An encryption manifest containing only font obfuscation does not require
  // this read path; let the reader open it normally.
  if (!book->isProtected()) return nullptr;

  // Loan enforcement against the system clock, which the boot-time NTP sync
  // sets. Validity is TimeUtils::isClockValid() — the firmware's single notion
  // of "we have a real time", the same one that puts the "?" on the status-bar
  // clock when the sync failed. The device has no battery-backed RTC, so an
  // unsynced cold boot sits near epoch 0, which reads as *before* any due date;
  // that must fail closed rather than quietly open an expired loan. The reader
  // then offers a Wi-Fi sync to resolve it.
  // Exact err strings below are matched by the reader for the user message.
  if (book->expiresAt() != 0) {
    if (!TimeUtils::isClockValid()) {
      err = "loan date unverified";
      return nullptr;
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    if (book->isExpired(now)) {
      err = "access expired";
      return nullptr;
    }
  }

  auto decryptor = makeUniqueNoThrow<ProtectedBookDecryptor>(epubPath, std::move(book));
  if (!decryptor) err = "out of memory";
  return decryptor;
}

}  // namespace content
}  // namespace freeink
