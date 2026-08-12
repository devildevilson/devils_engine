#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

#include "catalogue_domain.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/named_serializer.h"
#include "devils_engine/utils/sha256cpp.h"
#include "devils_engine/utils/time-utils.hpp"
#include "folder_module.h"
#include "module_interface.h"
#include "module_system.h"
#include "resource_system.h"
#include "zip_module.h"
namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {
namespace {

void hash_u64(utils::SHA256& hash, const uint64_t value) {
  std::array<uint8_t, sizeof(value)> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xffu);
  }
  hash.update(bytes.data(), bytes.size());
}

void hash_string(utils::SHA256& hash, const std::string_view value) {
  hash_u64(hash, value.size());
  hash.update(value.data(), value.size());
}

uint64_t hash_file_contents(utils::SHA256& hash, const fs::path& path) {
  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file) {
    utils::error{}("Could not open module file '{}' for fingerprinting", path.generic_string());
  }

  std::array<char, 64 * 1024> buffer{};
  uint64_t total = 0;
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = file.gcount();
    if (count > 0) {
      hash.update(buffer.data(), static_cast<size_t>(count));
      total += static_cast<uint64_t>(count);
    }
  }
  if (!file.eof()) {
    utils::error{}("Could not read module file '{}' for fingerprinting", path.generic_string());
  }
  return total;
}

std::string archive_fingerprint(const fs::path& path) {
  utils::SHA256 hash;
  (void)hash_file_contents(hash, path);
  return hash.hash();
}

std::string directory_fingerprint(const fs::path& root) {
  std::vector<fs::path> files;
  std::error_code ec;
  fs::recursive_directory_iterator itr(root, fs::directory_options::none, ec);
  const fs::recursive_directory_iterator end;
  if (ec) {
    utils::error{}("Could not enumerate module directory '{}': {}", root.generic_string(), ec.message());
  }

  while (itr != end) {
    const auto symlink_state = itr->symlink_status(ec);
    if (ec) {
      utils::error{}("Could not inspect module entry '{}': {}", itr->path().generic_string(), ec.message());
    }
    if (fs::is_symlink(symlink_state)) {
      utils::error{}("Module directory '{}' contains unsupported symlink '{}'", root.generic_string(), itr->path().generic_string());
    }
    if (fs::is_regular_file(symlink_state)) {
      files.push_back(itr->path().lexically_relative(root));
    }
    itr.increment(ec);
    if (ec) {
      utils::error{}("Could not enumerate module directory '{}': {}", root.generic_string(), ec.message());
    }
  }

  std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });

  utils::SHA256 hash;
  static constexpr std::string_view format = "devils_engine.demiurg.directory.v1";
  hash_string(hash, format);
  hash_u64(hash, files.size());
  for (const auto& relative : files) {
    const auto relative_string = relative.generic_string();
    const auto full_path = root / relative;
    const uint64_t file_size = fs::file_size(full_path, ec);
    if (ec) {
      utils::error{}("Could not query module file size '{}': {}", full_path.generic_string(), ec.message());
    }
    hash_string(hash, relative_string);
    hash_u64(hash, file_size);
    const uint64_t bytes_read = hash_file_contents(hash, full_path);
    if (bytes_read != file_size) {
      utils::error{}(
        "Module file '{}' changed while fingerprinting (expected {} bytes, read {})",
        full_path.generic_string(),
        file_size,
        bytes_read);
    }
  }
  return hash.hash();
}

std::string normalized_source(const std::string_view source) {
  return fs::path(source).lexically_normal().generic_string();
}

std::string canonical_module_id(const std::string_view source, const bool archive) {
  fs::path path(source);
  path = path.lexically_normal();
  if (archive) {
    path.replace_extension();
  }
  auto id = path.generic_string();
  while (id.starts_with("./")) {
    id.erase(0, 2);
  }
  while (!id.empty() && id.back() == '/') {
    id.pop_back();
  }
  if (id.empty() || path.is_absolute() || id == ".." || id.starts_with("../")) {
    utils::error{}("Module source '{}' does not produce a valid root-relative id", source);
  }
  return id;
}

std::string module_display_name(const std::string_view id) {
  return fs::path(id).filename().generic_string();
}

} // namespace

module_system::module_system(std::string path) : _path(std::move(path)) {
  rebuild_fingerprint();
}
module_system::~module_system() noexcept {}

std::string module_system::path() const {
  return _path;
}

std::string_view module_system::modules_list() const {
  return modules_list_name;
}

void module_system::set_modules_list(std::string modules_list) {
  modules_list_name = std::move(modules_list);
}

std::vector<module_system::list_entry> module_system::load_list(const std::string_view& list_name) const {
  // лист по умолчанию (чист релизная папка или архив)
  // нужно проверить... что берем по умолчанию? наверное файлик core.zip что то такое
  if (list_name.empty()) {
    const auto core_file = "core.zip";
    const auto core_folder = "core/";
    if (file_io::exists(path() + core_file)) {
      std::vector<list_entry> e{list_entry{core_file, "", ""}};
      return e;
    } else if (file_io::exists(path() + core_folder)) {
      std::vector<list_entry> e{list_entry{core_folder, "", ""}};
      return e;
    }

    utils::error{}("'{}' is not exist???", core_file);
  }

  const auto list_path = std::string(list_name) + ".json";
  if (!file_io::exists(path() + list_path)) {
    utils::error{}("File '{}' not exists", list_path);
  }

  const auto cont = file_io::read(path() + list_path);
  std::vector<list_entry> list_entries;
  const auto ec = utils::from_json(list_entries, cont);
  if (ec) {
    utils::error{}("Could not parse json '{}' for struct '{}' (err code: {})", list_path, "std::vector<list_entry>", static_cast<size_t>(ec.ec));
  }

  return list_entries;
}

void module_system::load_modules(std::vector<list_entry> paths) {
  modules.clear();
  loaded_module_list.clear();

  for (const auto& entry : paths) {
    const auto full_entry_path = path() + entry.path;
    const auto e = fs::directory_entry(full_entry_path);
    if (!e.exists()) {
      // ошибка? не, попробуем все равно загрузиться
      utils::warn("Could not find module '{}'", entry.path);
      continue;
    }

    const auto ftime = utils::file_timestamp(e);
    const auto datetime = utils::format_localtime(ftime, utils::ISO_datetime_format);

    const bool is_directory = e.is_directory();
    const auto extension = e.path().extension().generic_string();
    if (!is_directory && extension != ".mod" && extension != ".zip") {
      utils::warn("Module extension '{}' is not supported", extension);
      continue;
    }

    const auto source = normalized_source(entry.path);
    const auto id = canonical_module_id(entry.path, !is_directory);
    const auto duplicate = std::find_if(loaded_module_list.begin(), loaded_module_list.end(), [&](const loaded_module& loaded) {
      return loaded.id == id;
    });
    if (duplicate != loaded_module_list.end()) {
      utils::error{}("Duplicate logical module id '{}' from '{}' and '{}'", id, duplicate->source, source);
    }

    const auto actual_fingerprint = is_directory
      ? directory_fingerprint(e.path())
      : archive_fingerprint(e.path());
    if (!entry.hash.empty() && actual_fingerprint != entry.hash) {
      utils::warn(
        "Module '{}' mismatch (path: {})\nCur hash: {}\nExp hash: {}\nCur date: {}\nExp date: {}",
        id,
        entry.path,
        actual_fingerprint,
        entry.hash,
        datetime,
        entry.file_date);
    }

    const auto name = module_display_name(id);
    if (is_directory) {
      modules.push_back(std::make_unique<folder_module>(full_entry_path, name));
    } else {
      modules.push_back(std::make_unique<zip_module>(full_entry_path, name));
    }

    loaded_module_list.push_back(loaded_module{
      id,
      source,
      actual_fingerprint,
      is_directory ? module_kind::directory : module_kind::archive,
      static_cast<uint32_t>(loaded_module_list.size())});
  }

  rebuild_fingerprint();
}

void module_system::load_default_modules() {
  auto list = load_list(modules_list_name);
  load_modules(std::move(list));
}

void module_system::open_modules() {
  for (const auto& ptr : modules) {
    ptr->open();
  }
}

void module_system::close_modules() {
  for (const auto& ptr : modules) {
    ptr->close();
  }
}

void module_system::discover_resources(std::vector<resource_candidate>& out) {
  install_catalogue_introspection();
  using discover_t = catalogue_domain::fn_traits<&module_system::discover_resources_impl, "module_system.discover_resources", "self", "out">;
  discover_t::loc_fn_t{}(*this, out);
}

void module_system::discover_resources_impl(std::vector<resource_candidate>& out) {
  uint32_t priority = 0;
  for (const auto& ptr : modules) {
    ptr->resources_list(out, priority);
    priority += 1;
  }
}

std::span<const module_system::loaded_module> module_system::loaded_modules() const noexcept {
  return loaded_module_list;
}

std::string_view module_system::fingerprint() const noexcept {
  return modules_fingerprint;
}

void module_system::rebuild_fingerprint() {
  utils::SHA256 hash;
  static constexpr std::string_view format = "devils_engine.demiurg.module_set.v1";
  hash_string(hash, format);
  hash_u64(hash, fingerprint_format_version);
  hash_u64(hash, loaded_module_list.size());
  for (const auto& module : loaded_module_list) {
    hash_string(hash, module.id);
    hash_u64(hash, static_cast<uint8_t>(module.kind));
    hash_string(hash, module.fingerprint);
  }
  modules_fingerprint = hash.hash();
}
} // namespace demiurg
} // namespace devils_engine
