#include "fileio.h"

#include "core.h"
#include <filesystem>

namespace fs = std::filesystem;

namespace devils_engine {
namespace file_io {
template <>
std::vector<char> read(const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::in | std::ios::binary : std::ios::in;
  std::ifstream file(path, flags);
  if (!file) utils::error("Could not load file {}", path);
  return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

template <>
std::vector<uint8_t> read(const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::in | std::ifstream::ate | std::ios::binary : std::ios::in | std::ifstream::ate;
  std::ifstream file(path, flags);
  if (!file) utils::error("Could not load file {}", path);
  const size_t file_size = file.tellg();
  file.seekg(0);
  std::vector<uint8_t> ret(file_size, 0);
  file.read(reinterpret_cast<char *>(ret.data()), ret.size());
  return ret;
}

std::string read(const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::in | std::ios::binary : std::ios::in;
  std::ifstream file(path, flags);
  if (!file) utils::error("Could not load file {}", path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void write(const std::span<char> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  file.write(bytes.data(), bytes.size());
}

void write(const std::span<uint8_t> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write(const std::string &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  file << bytes;
}

void write(const std::span<const char> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  file.write(bytes.data(), bytes.size());
}

void write(const std::span<const uint8_t> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::out | std::ios::trunc;
  std::ofstream file(path, flags);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void append(const std::span<char> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  file.write(bytes.data(), bytes.size());
}

void append(const std::span<uint8_t> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void append(const std::string &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  file << bytes;
}

void append(const std::span<const char> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  file.write(bytes.data(), bytes.size());
}

void append(const std::span<const uint8_t> &bytes, const std::string &path, const enum type type) {
  const auto flags = type == type::binary ? std::ios::out | std::ios::binary | std::ios::app : std::ios::out | std::ios::app;
  std::ofstream file(path, flags);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool create_directory(const std::string &path) {
  return fs::create_directory(path);
}

bool exists(const std::string &path) noexcept {
  return fs::exists(path);
}

size_t size(const std::string &path) noexcept {
  std::error_code ec;
  const size_t size = fs::file_size(path, ec);
  if (ec) return 0;
  return size;
}

bool is_directory(const std::string &path) noexcept {
  return fs::is_directory(path);
}

bool is_regular_file(const std::string &path) noexcept {
  return fs::is_regular_file(path);
}
}
}