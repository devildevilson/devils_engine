#ifndef DEVILS_ENGINE_UTILS_COMPRESSION_H
#define DEVILS_ENGINE_UTILS_COMPRESSION_H

#include <cstddef>
#include <cstdint>
#include <vector>

// компрессия общего назначения через zstd (https://github.com/facebook/zstd) — системная,
// уже в дереве зависимостей (minizip-ng собран с MZ_ZSTD). Интерфейс стабилен: реализация в .cpp.

namespace devils_engine {
namespace utils {
enum class compression_level {
  fast,   // zstd 1  — быстрый, для сети
  normal, // zstd 3  — дефолт zstd
  high,   // zstd 12
  best,   // zstd 19 — для диска

  count
};

// на входе — исходный размер; безопасный размер выходного буфера.
size_t compress_safe_size(const size_t origin_size) noexcept;   // ZSTD_compressBound
size_t decompress_safe_size(const size_t origin_size) noexcept; // == origin_size (вызывающий знает сырой размер)

// возвращают число записанных байт либо SIZE_MAX при ошибке.
size_t compress(const uint8_t* input, const size_t input_size, uint8_t* output, const size_t output_size, const compression_level level) noexcept;
size_t decompress(const uint8_t* input, const size_t input_size, uint8_t* output, const size_t output_size) noexcept;

// vector-обёртки: compress по compressBound; decompress достаёт размер из zstd-фрейма.
// пустой вектор при ошибке.
std::vector<uint8_t> compress(const std::vector<uint8_t>& input, const compression_level level) noexcept;
std::vector<uint8_t> decompress(const std::vector<uint8_t>& input) noexcept;
} // namespace utils
} // namespace devils_engine

#endif
