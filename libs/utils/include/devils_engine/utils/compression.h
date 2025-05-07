#ifndef DEVILS_ENGINE_UTILS_COMPRESSION_H
#define DEVILS_ENGINE_UTILS_COMPRESSION_H

#include <cstdint>
#include <cstddef>
#include <vector>

// для компрессии общего назначения использую https://github.com/g1mv/density

namespace devils_engine {
namespace utils {
enum class compression_level {
  chameleon,
  cheetah,
  lion,

  count,

  fast = chameleon,
  normal = cheetah,
  best = lion
};

// вообще контекст как будто можно сохранять
// нужен ли мне compression_processing_result? не уверен
struct compression_processing_result {
  int32_t state;
  size_t bytesRead;
  size_t bytesWritten;
  void* context;
};

size_t compress_safe_size(const size_t origin_size) noexcept;
size_t decompress_safe_size(const size_t origin_size) noexcept;

// простой АПИ, наверное настраивать словарь мне не потребуется
size_t compress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size, const compression_level level) noexcept;
size_t decompress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size) noexcept;

std::vector<uint8_t> compress(const std::vector<uint8_t> &input, const compression_level level) noexcept;
std::vector<uint8_t> decompress(const std::vector<uint8_t> &input) noexcept;
}
}

#endif