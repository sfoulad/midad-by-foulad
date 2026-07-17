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

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;

  // Transient RAM copy of the 2bpp .pxc cache for this page view. A displayed
  // image page is re-rendered ~13x (BW pass + ~6 strip bands x 2 grayscale
  // planes), and the SD streaming path re-reads the whole cache file each pass
  // (~50KB x 13 = the dominant cost of an image page turn on device). Holding
  // the packed pixels in RAM makes passes 2..13 SD-free. Bounded and nothrow:
  // an alloc failure (or an image over the cap) silently keeps the SD path.
  // Freed automatically with the Page/block on page turn. Not serialized.
  static constexpr size_t RAM_CACHE_MAX_BYTES = 64 * 1024;
  std::unique_ptr<uint8_t[]> ramCache_;
  uint16_t ramCacheW_ = 0;
  uint16_t ramCacheH_ = 0;
  bool ramCacheTried_ = false;
};
