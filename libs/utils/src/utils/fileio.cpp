#include <filesystem>

#include "core.h"
#include "fileio.h"

namespace fs = std::filesystem;

namespace devils_engine {
namespace file_io {
namespace {

std::expected<void, read_error> check_regular(const std::string& path) {
  std::error_code code;
  const auto status = fs::status(path, code);
  if (status.type() == fs::file_type::not_found) {
    return std::unexpected(read_error::not_found);
  }
  if (code) {
    return std::unexpected(read_error::read_failed);
  }
  if (!fs::is_regular_file(status)) {
    return std::unexpected(read_error::not_regular);
  }
  return {};
}

} // namespace

std::string_view describe(const read_error error) noexcept {
  switch (error) {
    case read_error::not_found: return "not found";
    case read_error::not_regular: return "not a regular file";
    case read_error::read_failed: return "read failed";
  }
  return "unknown read error";
}

template <typename Byte>
std::expected<std::vector<Byte>, read_error> read(const std::string& path) {
  static_assert(sizeof(Byte) == 1);
  if (const auto checked = check_regular(path); !checked) {
    return std::unexpected(checked.error());
  }
  std::error_code code;
  const auto size = fs::file_size(path, code);
  if (code) {
    return std::unexpected(read_error::read_failed);
  }
  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file) {
    return std::unexpected(read_error::read_failed);
  }
  std::vector<Byte> out(static_cast<size_t>(size));
  if (size != 0) {
    file.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    if (size_t(file.gcount()) != out.size()) {
      return std::unexpected(read_error::read_failed);
    }
  }
  return out;
}

template std::expected<std::vector<std::byte>, read_error> read<std::byte>(const std::string&);
template std::expected<std::vector<uint8_t>, read_error> read<uint8_t>(const std::string&);
template std::expected<std::vector<char>, read_error> read<char>(const std::string&);

std::expected<std::string, read_error> read_text(const std::string& path) {
  if (const auto checked = check_regular(path); !checked) {
    return std::unexpected(checked.error());
  }
  std::ifstream file(path, std::ios::in);
  if (!file) {
    return std::unexpected(read_error::read_failed);
  }
  std::string out((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) {
    return std::unexpected(read_error::read_failed);
  }
  return out;
}

bool write(const std::span<char>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(bytes.data(), bytes.size());
  return true;
}

bool write(const std::span<uint8_t>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool write(const std::string& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file << bytes;
  return true;
}

bool write(const std::span<const char>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(bytes.data(), bytes.size());
  return true;
}

bool write(const std::span<const uint8_t>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool append(const std::span<char>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(bytes.data(), bytes.size());
  return true;
}

bool append(const std::span<uint8_t>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool append(const std::string& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file << bytes;
  return true;
}

bool append(const std::span<const char>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(bytes.data(), bytes.size());
  return true;
}

bool append(const std::span<const uint8_t>& bytes, const std::string& path, const enum type type) noexcept {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  if (!file) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

bool create_directory(const std::string& path) {
  return fs::create_directory(path);
}

bool exists(const std::string& path) noexcept {
  std::error_code code;
  return fs::exists(path, code);
}

size_t size(const std::string& path) noexcept {
  std::error_code ec;
  const size_t size = fs::file_size(path, ec);
  if (ec) {
    return 0;
  }
  return size;
}

bool is_directory(const std::string& path) noexcept {
  std::error_code code;
  return fs::is_directory(path, code);
}

bool is_regular_file(const std::string& path) noexcept {
  std::error_code code;
  return fs::is_regular_file(path, code);
}
} // namespace file_io
} // namespace devils_engine
