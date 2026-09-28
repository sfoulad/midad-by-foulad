#include "FouladContentHash.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <functional>

namespace {
constexpr char TAG[] = "CONTHASH";

// One SD read per iteration. 2KB rather than a larger block because this runs
// while a book is open and the reader's own allocations are already resident --
// see the heap notes in CLAUDE.md. Heap, not stack: the ESP32-C3's default stack
// cannot hold a buffer this size.
constexpr size_t READ_CHUNK = 2048;

// Same keying as every other per-book cache file (Epub.h: "epub_" + hash of the
// path), so a moved or renamed book re-derives rather than reusing a stale hash.
std::string sidecarPath(const std::string& bookPath) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(bookPath)) + "/sha256.txt";
}

// The cache stores "<size> <hex>". Size is the guard: a book edited in place
// keeps its path, so the hash alone cannot tell that the bytes changed.
bool readCache(const std::string& path, const size_t expectSize, std::string& outHex) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return false;

  char buf[96] = {};
  const int got = file.read(buf, sizeof(buf) - 1);
  if (got <= 0) return false;
  buf[got] = '\0';

  unsigned long long cachedSize = 0;
  char hex[65] = {};
  if (sscanf(buf, "%llu %64s", &cachedSize, hex) != 2) return false;
  if (cachedSize != expectSize) {
    LOG_DBG(TAG, "cache stale (size %llu != %zu)", cachedSize, expectSize);
    return false;
  }
  outHex = hex;
  return outHex.size() == 64;
}

void writeCache(const std::string& path, const size_t size, const std::string& hex) {
  char line[96];
  const int n = snprintf(line, sizeof(line), "%zu %s\n", size, hex.c_str());
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return;
  // The book's cache directory normally exists already (the metadata cache
  // creates it), but a book that has never been opened for reading has none.
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) Storage.ensureDirectoryExists(path.substr(0, slash).c_str());

  HalFile out;
  if (!Storage.openFileForWrite(TAG, path, out) ||
      out.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(n)) != static_cast<size_t>(n)) {
    // Not fatal: the hash is correct for this call, it just costs a full read
    // again next time.
    LOG_DBG(TAG, "could not cache hash to %s", path.c_str());
  }
}
}  // namespace

std::string FouladContentHash::get(const std::string& bookPath) {
  if (bookPath.empty()) return "";

  HalFile file;
  if (!Storage.openFileForRead(TAG, bookPath, file)) {
    LOG_ERR(TAG, "cannot open %s", bookPath.c_str());
    return "";
  }
  const size_t fileSize = file.fileSize();

  const std::string cachePath = sidecarPath(bookPath);
  std::string cached;
  if (readCache(cachePath, fileSize, cached)) {
    LOG_DBG(TAG, "cache hit: %s", cached.c_str());
    return cached;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(READ_CHUNK);
  if (!buffer) {
    LOG_ERR(TAG, "OOM: %zu bytes", READ_CHUNK);
    return "";
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts(&ctx, /*is224=*/0) != 0) {
    mbedtls_sha256_free(&ctx);
    LOG_ERR(TAG, "sha256 init failed");
    return "";
  }

  const uint32_t startedMs = millis();
  size_t total = 0;
  while (true) {
    const int got = file.read(buffer.get(), READ_CHUNK);
    if (got <= 0) break;
    if (mbedtls_sha256_update(&ctx, buffer.get(), static_cast<size_t>(got)) != 0) {
      mbedtls_sha256_free(&ctx);
      LOG_ERR(TAG, "sha256 update failed at %zu bytes", total);
      return "";
    }
    total += static_cast<size_t>(got);
    // A multi-megabyte book is thousands of iterations of SD read plus hashing;
    // without this the task watchdog fires long before the end.
    if ((total % (READ_CHUNK * 32)) == 0) vTaskDelay(1);
  }

  uint8_t digest[32];
  const int finished = mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (finished != 0) {
    LOG_ERR(TAG, "sha256 finish failed");
    return "";
  }

  char hex[65];
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  const std::string result(hex, 64);

  LOG_INF(TAG, "hashed %zu bytes in %lu ms: %s", total, static_cast<unsigned long>(millis() - startedMs),
          result.c_str());
  writeCache(cachePath, fileSize, result);
  return result;
}
