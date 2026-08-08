#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit and on any early epub release.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty for pre-lazy-extraction cache files
  int16_t width;
  int16_t height;

  static void* extractCtx;
  static ExtractFn extractFn;

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
