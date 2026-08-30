#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

class ReleaseJsonParser {
 public:
  // `assetName` is the release asset to pick out of the assets array. It must
  // outlive the parser — callers pass a string literal. Defaults to the C3
  // image so existing call sites and tests are unaffected.
  //
  // `releaseList` selects the payload shape. false (default) = a single release
  // object, i.e. GitHub's /releases/latest. true = the /releases ARRAY, used by
  // the pre-release channel because /releases/latest deliberately skips
  // prereleases. In list mode the parser walks the array in the order GitHub
  // returns it (newest created_at first) and keeps the FIRST release that
  // carries `assetName`, discarding the partial state of any earlier release
  // that had no matching asset; everything after that release is ignored.
  explicit ReleaseJsonParser(const char* assetName = "firmware.bin", bool releaseList = false);

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool foundTag() const;
  bool foundFirmware() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;

 private:
  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();
  // List mode only: called when one release object in the array closes. Either
  // locks in a release that yielded our asset, or clears the partial state so
  // the next (older) release is parsed from scratch.
  void finishRelease();

  StreamingJsonParser parser;

  // Not owned; a string literal supplied by the caller.
  const char* wantedAssetName;

  // Payload shape; fixed at construction, so reset() does not clear it.
  bool listMode;
  // List mode: the root array has been entered (its release objects sit at
  // depth 1, exactly where the single-object form puts the release's keys).
  bool inRootArray;
  // List mode: a release with a matching asset is locked in; ignore the rest.
  bool locked;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  char tagName[32];
  char firmwareUrl[512];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;

  char currentAssetName[32];
  char currentAssetUrl[512];
  size_t currentAssetSize;
};
