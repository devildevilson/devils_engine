#ifndef DEVILS_ENGINE_UTILS_FILEIO_H
#define DEVILS_ENGINE_UTILS_FILEIO_H

// Cross-platform filesystem and whole-file I/O helpers for project-owned paths.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace devils_engine {
namespace file_io {
enum class type {
  binary,
  text,
  count
};

// Почему файл не прочитан. Отсутствие файла — штатный исход для кэшей и необязательных
// настроек; что из этого критично, решает вызывающий.
enum class read_error : uint8_t {
  not_found,   // пути нет
  not_regular, // путь есть, но это не обычный файл (каталог, устройство)
  read_failed  // открыть или дочитать не удалось (права, файл изменился, ошибка ввода)
};
std::string_view describe(const read_error error) noexcept;

// Весь файл байтами, всегда в двоичном режиме. Byte — std::byte, uint8_t или char.
template <typename Byte = std::byte>
std::expected<std::vector<Byte>, read_error> read(const std::string& path);
// Весь файл текстом. В текстовом режиме Windows сводит CRLF к LF.
std::expected<std::string, read_error> read_text(const std::string& path);

bool write(const std::span<char>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool write(const std::span<uint8_t>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool write(const std::string& bytes, const std::string& path, const enum type type = type::text) noexcept;
bool write(const std::span<const char>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool write(const std::span<const uint8_t>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool append(const std::span<char>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool append(const std::span<uint8_t>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool append(const std::string& bytes, const std::string& path, const enum type type = type::text) noexcept;
bool append(const std::span<const char>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool append(const std::span<const uint8_t>& bytes, const std::string& path, const enum type type = type::binary) noexcept;
bool create_directory(const std::string& path);
bool exists(const std::string& path) noexcept;
size_t size(const std::string& path) noexcept;
bool is_directory(const std::string& path) noexcept;
bool is_regular_file(const std::string& path) noexcept;
} // namespace file_io
} // namespace devils_engine

#endif
