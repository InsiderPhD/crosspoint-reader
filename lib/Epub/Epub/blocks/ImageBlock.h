#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  // Cheap probe: is there a .pxc whose header matches this block's dimensions?
  // Opens the cache file but never reads the payload.
  bool hasValidCache() const;
  // True when rendering this image will cost a decode rather than a cache read,
  // i.e. it is worth showing a placeholder first.
  bool needsDecode() const;

  // Outline box in the image's reserved space. Drawn before a decode so the
  // page is readable while it runs, and again in place of any image that failed.
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;

  // Forget which images failed. Call once per page render: the failure list
  // exists to stop one bad image costing an SD open on each of the ~13 BW and
  // grayscale band passes, not to blank it permanently.
  static void clearRenderFailures();

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
