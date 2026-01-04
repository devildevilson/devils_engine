#ifndef DEVILS_ENGINE_UTILS_FILEIO_H
#define DEVILS_ENGINE_UTILS_FILEIO_H

#include <cstdint>
#include <cstddef>
#include <fstream>
#include <vector>
#include <string>
#include <string_view>
#include <span>

namespace devils_engine {
namespace file_io {
enum class type {
  binary,
  text,
  count
};

template <typename BYTE_TYPE = uint8_t>
std::vector<BYTE_TYPE> read(const std::string &path, const enum type type = type::binary) noexcept;
std::string read(const std::string &path, const enum type type = type::text) noexcept;
bool write(const std::span<char> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool write(const std::span<uint8_t> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool write(const std::string &bytes, const std::string &path, const enum type type = type::text) noexcept;
bool write(const std::span<const char> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool write(const std::span<const uint8_t> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool append(const std::span<char> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool append(const std::span<uint8_t> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool append(const std::string &bytes, const std::string &path, const enum type type = type::text) noexcept;
bool append(const std::span<const char> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool append(const std::span<const uint8_t> &bytes, const std::string &path, const enum type type = type::binary) noexcept;
bool create_directory(const std::string &path);
bool exists(const std::string &path) noexcept;
size_t size(const std::string &path) noexcept;
bool is_directory(const std::string &path) noexcept;
bool is_regular_file(const std::string &path) noexcept;
}
}

#endif