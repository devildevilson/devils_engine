#include "core/content_root.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/network/session.h>

namespace frontier_online {
namespace core {

namespace demiurg = devils_engine::demiurg;
namespace network = devils_engine::network;
namespace utils = devils_engine::utils;

namespace {

struct manifest_file {
  std::string package; // модуль ресурса
  std::string path;    // относительно модуля, с '/' как разделителем
  std::vector<std::byte> bytes;
};

} // namespace

bool build_causal_content_root(const demiurg::resource_system& resources,
                               causal_content_manifest& output, std::string& detail) {
  std::vector<manifest_file> files;
  std::vector<demiurg::resource_interface*> found;
  for (const auto directory : causal_content_directories) {
    const std::string prefix = std::string(directory) + "/";
    found.clear();
    resources.directory<demiurg::resource_interface>(prefix, found);
    if (found.empty()) {
      // Отказ, а не пропуск. Пустой причинный каталог — это либо неполная установка, либо
      // переименование, о котором забыли здесь; и то и другое обязано остановить присоединение.
      detail = "declared causal directory '" + prefix + "' holds no resources";
      return false;
    }
    for (const auto* resource : found) {
      if (resource->module == nullptr) {
        detail = "resource '" + std::string(resource->id) + "' has no module to read from";
        return false;
      }
      const std::string package(resource->module_name);
      const bool seen = std::any_of(files.begin(), files.end(), [&](const manifest_file& file) {
        return file.package == package && file.path == resource->path;
      });
      if (seen) continue; // под-ресурс уже учтённого файла
      manifest_file file;
      file.package = package;
      file.path = resource->path;
      std::vector<char> raw;
      resource->module->load_binary(resource->path, raw);
      file.bytes.resize(raw.size());
      std::memcpy(file.bytes.data(), raw.data(), raw.size());
      files.push_back(std::move(file));
    }
  }

  // Канонический порядок: по модулю и пути. Порядок обхода реестра зависит от типов и замен —
  // поэтому манифест сортируется здесь, а не принимается в порядке выдачи.
  std::ranges::sort(files, [](const manifest_file& a, const manifest_file& b) {
    return a.package != b.package ? a.package < b.package : a.path < b.path;
  });

  std::vector<network::session_content_entry> entries;
  entries.reserve(files.size());
  size_t total = 0;
  for (const auto& file : files) {
    network::session_content_entry entry;
    entry.domain = network::session_content_domain::core;
    entry.load_order = 0;
    entry.package = file.package;
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
