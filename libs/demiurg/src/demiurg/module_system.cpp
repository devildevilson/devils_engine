#include "module_system.h"

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/time-utils.hpp"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/sha256cpp.h"
#include "devils_engine/utils/named_serializer.h"
#include "module_interface.h"
#include "folder_module.h"
#include "zip_module.h"
#include "resource_system.h"

#include <filesystem>
namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {
module_system::module_system(std::string path) noexcept : _path(std::move(path)) {}
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

static std::tuple<std::string_view, std::string_view> get_name_ext(const std::string_view &path) {
  const auto ext = path.substr(path.rfind('.'));
  const auto name = path.substr(0, path.rfind('.')).substr(path.rfind('/')+1);
  return std::make_tuple(name, ext);
}

std::vector<module_system::list_entry> module_system::load_list(const std::string_view &list_name) const {
  // лист по умолчанию (чист релизная папка или архив)
  // нужно проверить... что берем по умолчанию? наверное файлик core.zip что то такое
  if (list_name.empty()) {
    const auto core_file = "core.zip";
    const auto core_folder = "core/";
    if (file_io::exists(path() + core_file)) {
      std::vector<list_entry> e{ list_entry{ core_file, "", "" } };
      return e;
    } else if (file_io::exists(path() + core_folder)) {
      std::vector<list_entry> e{ list_entry{ core_folder, "", "" } };
      return e;
    } 
        
    utils::error{}("'{}' is not exist???", core_file);
  }

  const auto list_path = std::string(list_name) + ".json";
  if (!file_io::exists(path() + list_path)) utils::error{}("File '{}' not exists", list_path);

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

  // как быть с путями
  for (const auto &entry : paths) {
    const auto full_entry_path = path() + entry.path;
    const auto e = fs::directory_entry(full_entry_path);
    if (!e.exists()) {
      // ошибка? не, попробуем все равно загрузиться
      utils::warn("Could not find module '{}'", entry.path);
      continue;
    }

    const auto ftime = utils::file_timestamp(e);
    const auto datetime = utils::format_localtime(ftime, utils::ISO_datetime_format);

    if (!e.is_directory()) {
      const auto [name, ext] = get_name_ext(full_entry_path);
      const auto cont = file_io::read<uint8_t>(full_entry_path);
      const auto hash = utils::SHA256::easy(cont.data(), cont.size());
      if (!entry.hash.empty() && hash != entry.hash) {
        utils::warn("Module '{}' mismatch (path: {})\nCur hash: {}\nExp hash: {}\nCur date: {}\nExp date: {}", name, entry.path, hash, entry.hash, datetime, entry.file_date);
      }
    }

    if (e.is_directory()) {
      auto mem = std::make_unique<folder_module>(full_entry_path);
      modules.push_back(std::move(mem));
    } else {
      const auto [name, ext] = get_name_ext(full_entry_path);
      // тут надо проверить ext, в принципе наверное тут будет только .mod и .zip
      if (ext == "mod" || ext == "zip") {
        auto mem = std::make_unique<zip_module>(full_entry_path);
        modules.push_back(std::move(mem));
      } else {
        // (сюда попасть мы никак не должны)
        utils::warn("Module extension '{}' is not supported", ext);
        continue;
      }
    }
  }
}

void module_system::load_default_modules() {
  auto list = load_list(modules_list_name);
  load_modules(std::move(list));
}

void module_system::open_modules() {
  for (const auto &ptr : modules) { ptr->open(); }
}

void module_system::close_modules() {
  for (const auto &ptr : modules) { ptr->close(); }
}

void module_system::parse_resources(resource_system* sys) {
  // тут запускаем новую функцию resources_list
  for (const auto &ptr : modules) {
    ptr->resources_list(sys);
  }
}
}
}