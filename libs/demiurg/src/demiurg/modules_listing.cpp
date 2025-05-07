#include "modules_listing.h"

#include <filesystem>
#include "utils/named_serializer.h"
#include "utils/fileio.h"
#include "utils/time-utils.hpp"
#include "utils/sha256cpp.h"

namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {
modules_listing::modules_listing(std::string root) noexcept : root_path(root) {}
modules_listing::~modules_listing() noexcept { clear(); }

void modules_listing::clear() {
  // тут в будущем нужно будет удалить ресурсы
  entries_arr.clear();
  lists.clear();
}

static std::tuple<std::string_view, std::string_view> get_name_ext(const std::string_view &path) {
  const auto ext = path.substr(path.rfind('.'));
  const auto name = path.substr(0, path.rfind('.')).substr(path.rfind('/')+1);
  return std::make_tuple(name, ext);
}

// заново загрузим файлы в папке + листы
void modules_listing::reload() {
  clear();

  for (const auto &entry : fs::directory_iterator(root_path)) {
    std::string entry_path = entry.path().generic_string();
    const auto [ name, ext ] = get_name_ext(entry_path);
    if (ext == ".json") continue; // грузим позже

    if (!entry.is_directory()) {
      // это мод в зипе
      //if (ext == ".mod" || ext == ".zip") 
        load_mod(entry);
      //else {
      //  utils::warn("Extension '{}' is not supported", ext);
      //  continue;
      //}
    }

    if (entry.is_directory()) {
      // это мод в папке (например еще не опубликованный)
      load_mod(entry);
    }
  }

  for (const auto &entry : fs::directory_iterator(root_path)) {
    std::string entry_path = entry.path().generic_string();
    const auto [ name, ext ] = get_name_ext(entry_path);
    if (ext == ".json") {
      load_list(entry_path);
    }
  }
}

void modules_listing::save_list(const std::string &name, const std::vector<view> &entries) const {
  std::vector<list_entry> data(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &e = entries[i];
    data[i] = list_entry{ e.path, e.hash, e.ptr->file_date };
  }

  const auto cont = utils::to_json(data);
  file_io::write(cont.value(), root_path + name + ".json");
}

void modules_listing::save_list(const std::string &name, const std::vector<std::string> &paths) const {
  std::vector<list_entry> data(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    const auto &str = paths[i];
    const auto f = std::find_if(entries_arr.begin(), entries_arr.end(), [&str] (const auto &a) -> bool { return a->path == str; });
    data[i] = list_entry{ f->get()->path, f->get()->hash, f->get()->file_date };
  }

  const auto cont = utils::to_json(data);
  file_io::write(cont.value(), root_path + name + ".json");
}

modules_listing::list modules_listing::load_list(const std::string &name) const {
  // по идее все листы мы подгрузим в reload(), можно тут подгрузить что то конкретное без запоминания
  const auto path = root_path + name + ".json";
  if (!file_io::exists(path)) utils::error("File '{}' not exists", path);

  const auto cont = file_io::read(path);
  std::vector<list_entry> list_entries;
  const auto ec = utils::from_json(list_entries, cont);
  if (ec) {
    utils::error("Could not parse json '{}' for struct '{}' (err code: {})", path, "std::vector<list_entry>", static_cast<size_t>(ec.ec));
  }

  list l;
  l.name = name;
  for (const auto &e : list_entries) {
    const auto f = std::find_if(entries_arr.begin(), entries_arr.end(), [&e] (const auto &a) -> bool { return a->path == e.path; });
    l.entries.push_back({ e.path, e.hash, f->get() });
  }

  return l;
}

const std::vector<modules_listing::list> & modules_listing::available_lists() const {
  return lists;
}

std::vector<std::string> modules_listing::available_lists_names() const {
  std::vector<std::string> lists;
  for (const auto &entry : fs::directory_iterator(root_path)) {
    std::string entry_path = entry.path().generic_string();
    const auto [ name, ext ] = get_name_ext(entry_path);
    if (ext == ".json") {
      lists.push_back(std::string(name));
    }
  }

  return lists;
}

const std::vector<std::unique_ptr<modules_listing::module_entry>> & modules_listing::entries() const {
  return entries_arr;
}

void modules_listing::load_list(const std::string &path) {
  const auto [ name, ext ] = get_name_ext(path);
  const auto cont = file_io::read(path);
  std::vector<list_entry> list_entries;
  const auto ec = utils::from_json(list_entries, cont);
  if (ec) {
    // вылетать тут ни к чему, просто пропускаем файл
    utils::warn("Could not parse json '{}' for struct '{}' (err code: {}). Skip", path, "std::vector<list_entry>", static_cast<size_t>(ec.ec));
    return;
  }

  list l;
  l.name = std::string(name);
  size_t count = 0;
  for (const auto &e : list_entries) {
    if (e.path.empty()) {
      utils::warn("Invalid list '{}' entry {} structure. Skip this entry", name, count);
      continue;
    }

    const auto f = std::find_if(entries_arr.begin(), entries_arr.end(), [&e] (const auto &a) -> bool { return a->path == e.path; });
    l.entries.push_back({ e.path, e.hash, f->get() });
    count += 1;
  }

  lists.push_back(l);
}

void modules_listing::load_mod(const fs::directory_entry &entry) {
  const auto path = entry.path().generic_string();
  const auto [ name, ext ] = get_name_ext(path);

  const auto ftime = utils::file_timestamp(entry);
  const auto datetime = utils::format_localtime(ftime, utils::ISO_datetime_format);

  std::string hash;
  if (!entry.is_directory()) {
    const auto cont = file_io::read<uint8_t>(path);
    hash = utils::SHA256::easy(cont.data(), cont.size());
  }

  std::unique_ptr<module_entry> md(new module_entry{ path, std::string(name), hash, 0, ftime, datetime, "" });
  entries_arr.push_back(std::move(md));
}
}
}