#include "core/content_root.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include <devils_engine/network/session.h>

namespace frontier_online {
namespace core {

namespace network = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

struct manifest_file {
  std::string path; // относительно модуля, с '/' как разделителем
  std::vector<std::byte> bytes;
};

bool read_whole_file(const std::filesystem::path& path, std::vector<std::byte>& out) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0) return false;
  input.seekg(0, std::ios::beg);
  out.resize(size_t(size));
  if (size != 0) input.read(reinterpret_cast<char*>(out.data()), size);
  return bool(input);
}

} // namespace

bool build_causal_content_root(const std::string_view resource_root,
                               const std::string_view module_name,
                               causal_content_manifest& output, std::string& detail) {
  namespace fs = std::filesystem;

  const fs::path module_root = fs::path(std::string(resource_root)) / std::string(module_name);
  std::error_code code;
  if (!fs::is_directory(module_root, code)) {
    detail = "module directory '" + module_root.string() + "' does not exist";
    return false;
  }

  std::vector<manifest_file> files;
  for (const auto directory : causal_content_directories) {
    const fs::path branch = module_root / std::string(directory);
    if (!fs::is_directory(branch, code)) {
      // Отказ, а не пропуск. Отсутствующий причинный каталог — это либо неполная установка, либо
      // переименование, о котором забыли здесь; и то и другое обязано остановить присоединение.
      detail = "declared causal directory '" + branch.string() + "' is missing";
      return false;
    }
    size_t found = 0;
    for (const auto& entry : fs::recursive_directory_iterator(branch, code)) {
      if (!entry.is_regular_file()) continue;
      manifest_file file;
      file.path = fs::relative(entry.path(), module_root, code).generic_string();
      if (code) {
        detail = "cannot express '" + entry.path().string() + "' relative to the module";
        return false;
      }
      if (!read_whole_file(entry.path(), file.bytes)) {
        detail = "cannot read '" + entry.path().string() + "'";
        return false;
      }
      files.push_back(std::move(file));
      ++found;
    }
    if (found == 0) {
      detail = "declared causal directory '" + branch.string() + "' holds no files";
      return false;
    }
  }

  // Канонический порядок: по пути. Обход файловой системы его не даёт и давать не обязан —
  // именно поэтому манифест сортируется здесь, а не принимается в порядке выдачи каталога.
  std::ranges::sort(files, [](const manifest_file& a, const manifest_file& b) {
    return a.path < b.path;
  });

  std::vector<network::session_content_entry> entries;
  entries.reserve(files.size());
  size_t total = 0;
  for (const auto& file : files) {
    network::session_content_entry entry;
    entry.domain = network::session_content_domain::core;
    entry.load_order = 0;
    entry.package = module_name;
    entry.path = file.path;
    entry.bytes = std::span<const std::byte>(file.bytes);
    entries.push_back(entry);
    total += file.bytes.size();
  }

  network::session_content_manifest manifest;
  manifest.product = "frontier_online";
  manifest.version = "0.0.1";
  manifest.entries = entries;

  utils::digest root{};
  const auto status = network::try_make_session_content_root(manifest, root);
  if (status != network::session_content_status::built) {
    detail = "manifest refused, status " + std::to_string(unsigned(status));
    return false;
  }

  output.root = root;
  output.files = files.size();
  output.bytes = total;
  return true;
}

std::string short_digest(const utils::digest& value) {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::string text;
  text.reserve(16);
  for (size_t i = 0; i < 8; ++i) {
    text.push_back(alphabet[(value[i] >> 4) & 0xf]);
    text.push_back(alphabet[value[i] & 0xf]);
  }
  return text;
}

} // namespace core
} // namespace frontier_online
