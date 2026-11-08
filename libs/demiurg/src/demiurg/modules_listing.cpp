#include "devils_engine/utils/log.h"
#include "modules_listing.h"

#include <algorithm>
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/sha256cpp.h"
#include "devils_engine/utils/time-utils.hpp"

// Каталог хранит root-relative пути, как module_system. Profiles читаются одним TAVL codec;
// view действителен до clear/reload, а save проверяет его принадлежность текущему каталогу.
namespace devils_engine::demiurg {
namespace {
std::string path_key(const std::string& path) {
  auto key = fs::path(path).lexically_normal().generic_string();
  while (key.ends_with('/'))
    key.pop_back();
  return key;
}
std::vector<fs::directory_entry> directory(const std::string& root) {
  std::error_code error;
  fs::directory_iterator it(root, error), end;
  if (error) utils::error{}("Could not list module root '{}': {}", root, error.message());
  std::vector<fs::directory_entry> result;
  while (it != end) {
    result.push_back(*it);
    it.increment(error);
    if (error) utils::error{}("Could not enumerate module root '{}': {}", root, error.message());
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return a.path() < b.path();
  });
  return result;
}
} // namespace

modules_listing::modules_listing(std::string root) noexcept : root_path(std::move(root)) {}
modules_listing::~modules_listing() noexcept = default;

void modules_listing::clear() {
  lists.clear();
  entries_arr.clear();
}

void modules_listing::reload() {
  clear();
  const auto files = directory(root_path);
  for (const auto& entry : files) {
    const auto extension = entry.path().extension();
    if (entry.is_directory() || extension == ".zip" || extension == ".mod")
      load_mod(entry);
    else if (extension == ".json")
      utils::warn("Module profile '{}' is legacy JSON; use scripts/convert_module_profile.py", entry.path().generic_string());
  }
  for (const auto& entry : files) {
    if (entry.is_regular_file() && entry.path().extension() == ".tavl") {
      lists.push_back(static_cast<const modules_listing&>(*this).load_list(entry.path().filename().generic_string()));
    }
  }
}

void modules_listing::save_list(const std::string& name, const std::vector<view>& entries) const {
  module_profile profile;
  for (const auto& view : entries) {
    const auto found = std::find_if(entries_arr.begin(), entries_arr.end(), [&](const auto& entry) {
      return path_key(entry->path) == path_key(view.path);
    });
    if (found == entries_arr.end()) utils::error{}("Module profile '{}': module '{}' is not installed", name, view.path);
    if (view.ptr != found->get()) utils::error{}("Module profile '{}': view for '{}' is stale or belongs to another listing", name, view.path);
    profile.modules.push_back({(*found)->path, view.hash, (*found)->file_date});
  }
  write_module_profile(module_profile_path(root_path, name), profile);
}

void modules_listing::save_list(const std::string& name, const std::vector<std::string>& paths) const {
  module_profile profile;
  for (const auto& path : paths) {
    const auto found = std::find_if(entries_arr.begin(), entries_arr.end(), [&](const auto& entry) {
      return path_key(entry->path) == path_key(path);
    });
    if (found == entries_arr.end()) utils::error{}("Module profile '{}': module '{}' is not installed", name, path);
    profile.modules.push_back({(*found)->path, (*found)->hash, (*found)->file_date});
  }
  write_module_profile(module_profile_path(root_path, name), profile);
}

modules_listing::list modules_listing::load_list(const std::string& name) const {
  const auto path = module_profile_path(root_path, name);
  const auto profile = read_module_profile(path);
  list result;
  result.name = path.stem().generic_string();
  for (const auto& item : profile.modules) {
    const auto found = std::find_if(entries_arr.begin(), entries_arr.end(), [&](const auto& entry) {
      return path_key(entry->path) == path_key(item.path);
    });
    if (found == entries_arr.end()) utils::error{}("Module profile '{}': module '{}' is not installed", path.generic_string(), item.path);
    result.entries.push_back({(*found)->path, item.hash, found->get()});
  }
  return result;
}

const std::vector<modules_listing::list>& modules_listing::available_lists() const {
  return lists;
}
const std::vector<std::unique_ptr<modules_listing::module_entry>>& modules_listing::entries() const {
  return entries_arr;
}

std::vector<std::string> modules_listing::available_lists_names() const {
  std::vector<std::string> names;
  for (const auto& entry : directory(root_path)) {
    if (entry.is_regular_file() && entry.path().extension() == ".tavl") names.push_back(entry.path().stem().generic_string());
  }
  return names;
}

void modules_listing::load_mod(const fs::directory_entry& entry) {
  const auto path = entry.path();
  const bool folder = entry.is_directory();
  const auto timestamp = utils::file_timestamp(entry);
  std::string hash;
  if (!folder) {
    const auto data = file_io::read<uint8_t>(path.generic_string());
    hash = utils::SHA256::easy(data.data(), data.size());
  }
  auto relative = path.filename().generic_string();
  if (folder) relative += '/';
  entries_arr.push_back(std::make_unique<module_entry>(module_entry{
    relative, path.stem().generic_string(), std::move(hash), 0, timestamp,
    utils::format_localtime(timestamp, utils::ISO_datetime_format), ""}));
}
} // namespace devils_engine::demiurg
