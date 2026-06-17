#include "BookFusionMetaStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

namespace {
constexpr char MODULE[] = "BFMETA";
constexpr char SIDECAR_NAME[] = "/bf_meta.bin";
constexpr uint8_t META_VERSION = 1;
}  // namespace

bool BookFusionMetaStore::save(const std::string& cacheDir, const BookFusionMeta& meta) {
  const std::string path = cacheDir + SIDECAR_NAME;

  // Nothing worth persisting -> remove any stale sidecar and succeed.
  if (meta.empty()) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    return true;
  }

  FsFile file;
  if (!Storage.openFileForWrite(MODULE, path, file)) {
    LOG_ERR(MODULE, "Could not write BookFusion metadata sidecar");
    return false;
  }
  serialization::writePod(file, META_VERSION);
  serialization::writeString(file, meta.categories);
  serialization::writeString(file, meta.bookshelves);
  serialization::writeString(file, meta.lists);
  file.close();
  return true;
}

bool BookFusionMetaStore::load(const std::string& cacheDir, BookFusionMeta& out) {
  const std::string path = cacheDir + SIDECAR_NAME;
  FsFile file;
  if (!Storage.openFileForRead(MODULE, path, file)) {
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(file, version);
  if (version != META_VERSION) {
    file.close();
    return false;
  }
  serialization::readString(file, out.categories);
  serialization::readString(file, out.bookshelves);
  serialization::readString(file, out.lists);
  file.close();
  return true;
}
