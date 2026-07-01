#include "compression.h"

#include <zstd.h>

namespace devils_engine {
namespace utils {
namespace {
int zstd_level(const compression_level level) noexcept {
  switch (level) {
    case compression_level::fast:   return 1;
    case compression_level::normal: return 3;
    case compression_level::high:   return 12;
    case compression_level::best:   return 19;
    default:                        return 3;
  }
}
} // namespace

size_t compress_safe_size(const size_t origin_size) noexcept { return ZSTD_compressBound(origin_size); }
// zstd знает точный размер (мы передаём сырой из заголовка снапшота) — запас не нужен.
size_t decompress_safe_size(const size_t origin_size) noexcept { return origin_size; }

size_t compress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size, const compression_level level) noexcept {
  const size_t r = ZSTD_compress(output, output_size, input, input_size, zstd_level(level));
  if (ZSTD_isError(r)) return SIZE_MAX;
  return r;
}

size_t decompress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size) noexcept {
  const size_t r = ZSTD_decompress(output, output_size, input, input_size);
  if (ZSTD_isError(r)) return SIZE_MAX;
  return r;
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &input, const compression_level level) noexcept {
  std::vector<uint8_t> out(compress_safe_size(input.size()), 0);
  const size_t bytes_written = compress(input.data(), input.size(), out.data(), out.size(), level);
  if (bytes_written == SIZE_MAX) return std::vector<uint8_t>();
  out.resize(bytes_written);
  return out;
}

std::vector<uint8_t> decompress(const std::vector<uint8_t> &input) noexcept {
  // размер несжатых данных зашит в zstd-фрейм (single-shot ZSTD_compress его пишет).
  const unsigned long long content = ZSTD_getFrameContentSize(input.data(), input.size());
  if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN) return std::vector<uint8_t>();
  std::vector<uint8_t> out(size_t(content), 0);
  const size_t bytes_written = decompress(input.data(), input.size(), out.data(), out.size());
  if (bytes_written == SIZE_MAX) return std::vector<uint8_t>();
  out.resize(bytes_written);
  return out;
}
}
}
