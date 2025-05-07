#include "compression.h"
#include "density_api.h"

namespace devils_engine {
namespace utils {
size_t compress_safe_size(const size_t origin_size) noexcept { return density_compress_safe_size(origin_size); }
size_t decompress_safe_size(const size_t origin_size) noexcept { return density_decompress_safe_size(origin_size); }

// простой АПИ, наверное настраивать словарь мне не потребуется
size_t compress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size, const compression_level level) noexcept {
  const auto res = density_compress(input, input_size, output, output_size, static_cast<DENSITY_ALGORITHM>(level));
  if (res.state != DENSITY_STATE_OK) return SIZE_MAX;
  return res.bytesWritten;
}

size_t decompress(const uint8_t *input, const size_t input_size, uint8_t *output, const size_t output_size) noexcept {
  const auto res = density_decompress(input, input_size, output, output_size);
  if (res.state != DENSITY_STATE_OK) return SIZE_MAX;
  return res.bytesWritten;
}

std::vector<uint8_t> compress(const std::vector<uint8_t> &input, const compression_level level) noexcept {
  std::vector<uint8_t> out(compress_safe_size(input.size()), 0);
  const size_t bytes_written = compress(input.data(), input.size(), out.data(), out.size(), level);
  if (bytes_written == SIZE_MAX) return std::vector<uint8_t>();
  out.resize(bytes_written);
  return out;
}

std::vector<uint8_t> decompress(const std::vector<uint8_t> &input) noexcept {
  std::vector<uint8_t> out(decompress_safe_size(input.size()), 0);
  const size_t bytes_written = decompress(input.data(), input.size(), out.data(), out.size());
  if (bytes_written == SIZE_MAX) return std::vector<uint8_t>();
  out.resize(bytes_written);
  return out;
}
}
}