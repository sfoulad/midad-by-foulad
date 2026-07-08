#pragma once

#include <HalStorage.h>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(HalFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                         bool oneBit, bool crop = true, bool allowUpscale = true);

 public:
  static bool pngFileToBmpStream(HalFile& pngFile, Print& bmpOut, bool crop = true);
  static bool pngFileToBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // allowUpscale=false keeps a source smaller than targetMaxWidth/Height at its own native size
  // (never stretched up to fill the target) -- used for OPDS covers, where the server already
  // serves small source covers at native size rather than padding them, so upscaling here would
  // just blur them for no benefit.
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool allowUpscale = true);
};
