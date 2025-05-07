#include "zip_module.h"

#include "utils/core.h"

#include "resource_system.h"

//#include "utils/mz.h"
//#include "utils/mz_zip.h"
//#include "utils/mz_strm.h"
//#include "utils/mz_zip_rw.h"
#include "mz.h"
#include "mz_zip.h"
#include "mz_strm.h"
#include "mz_zip_rw.h"

namespace devils_engine {
namespace demiurg {
zip_module::zip_module(std::string root) noexcept : module_interface(std::move(root)), native_handle(nullptr) {
  const auto view = std::string_view(_path);
  module_name = view.substr(view.rfind('/')+1).substr(0, view.rfind('.'));
}

zip_module::~zip_module() noexcept { close(); }

std::string_view zip_module::name() const { return module_name; }

void zip_module::open() {
  native_handle = mz_zip_reader_create();
  const auto err = mz_zip_reader_open_file(native_handle, _path.c_str());
  if (err != MZ_OK) utils::error("Could not open archive '{}'", _path);
}
void zip_module::close() {
  if (native_handle == nullptr) return;
  const auto err = mz_zip_reader_close(native_handle);
  if (err != MZ_OK) utils::error("Could not close archive '{}'", _path);
  mz_zip_reader_delete(&native_handle);
  native_handle = nullptr;
}

bool zip_module::is_openned() const { return native_handle != nullptr; }

// тут бы желательно взять путь, убрать часть пути папки и парсить чисто остаток
static std::tuple<std::string_view, std::string_view, std::string_view> parse_path(const std::string_view &path) {
  // раньше я брал file_name как оносительный путь от папки
  // теперь наверное проще сделать по другому
  const size_t last_slash = path.rfind('/');
  const size_t last_dot = path.rfind('.');
  // здесь не должна приходить папка, должно ли у файлов быть какое то разрешение в принципе?
  // да именно по разрешению мы понимаем что к чему относится 
  const auto ext = last_dot != std::string_view::npos ? path.substr(last_dot+1) : std::string_view();
  const auto id = path.substr(0, last_dot);
  const auto name = path.substr(last_slash+1).substr(0, last_dot);
  return std::make_tuple(id, name, ext);
}

// зачем я массив передаю? черт его знает я все равно завишу от указателя на систему
// как будто как раз тут нужно передавать указатель на систему, где мы и создадим новый ресурс...
// ну да выглядит как недостаток интерфейса
//void zip_module::resources_list(std::vector<resource_interface*> &arr) const {
//  int32_t err = MZ_OK;
//  err = mz_zip_reader_goto_first_entry(native_handle);
//  while (err == MZ_OK) {
//    const int32_t is_dir = mz_zip_reader_entry_is_dir(native_handle);
//
//    if (is_dir == MZ_OK) {
//      err = mz_zip_reader_goto_next_entry(native_handle);
//      continue;
//    }
//
//    mz_zip_file *file_info = nullptr;
//    auto cur = mz_zip_reader_entry_get_info(native_handle, &file_info);
//    if (cur != MZ_OK) utils::error("Could not get entry from archive '{}'", _path);
//
//    utils::println(file_info->filename, file_info->uncompressed_size);
//    std::string file_path = file_info->filename;
//
//    const auto [ id, name, ext ] = parse_path(file_path);
//    if (name == "." || name == "..") continue;
//
//    auto t = sys->find_type(id, ext);
//
//    if (t == nullptr) {
//      utils::warn("Could not find proper resource type for file '{}'. Skip", file_path);
//      continue;
//    }
//
//    auto res = t->create();
//    res->set(std::move(file_path), module_name, id, ext);
//    res->module = this;
//    res->raw_size = file_info->uncompressed_size;
//    arr.push_back(res);
//
//    err = mz_zip_reader_goto_next_entry(native_handle);
//  }
//}

void zip_module::resources_list(resource_system* s) const {
  int32_t err = MZ_OK;
  err = mz_zip_reader_goto_first_entry(native_handle);
  while (err == MZ_OK) {
    const int32_t is_dir = mz_zip_reader_entry_is_dir(native_handle);

    if (is_dir == MZ_OK) {
      err = mz_zip_reader_goto_next_entry(native_handle);
      continue;
    }

    mz_zip_file *file_info = nullptr;
    auto cur = mz_zip_reader_entry_get_info(native_handle, &file_info);
    if (cur != MZ_OK) utils::error("Could not get entry from archive '{}'", _path);

    //utils::println(file_info->filename, file_info->uncompressed_size);
    std::string file_path = file_info->filename;

    const auto [ id, name, ext ] = parse_path(file_path);
    if (name == "." || name == "..") continue;

    auto res = s->create(id, ext);
    res->set(std::move(file_path), module_name, id, ext);
    res->module = this;
    res->raw_size = file_info->uncompressed_size;

    err = mz_zip_reader_goto_next_entry(native_handle);
  }
}

void zip_module::load_binary(const std::string &path, std::vector<uint8_t> &mem) const {
  int32_t err = 0;
  err = mz_zip_reader_locate_entry(native_handle, path.c_str(), 0);
  // мы можем не найти энтри?
  if (err != MZ_OK) utils::error("Could not get entry '{}' from archive '{}'", path, _path);

  err = mz_zip_reader_entry_open(native_handle);
  if (err != MZ_OK) utils::error("Could not open entry '{}' from archive '{}'", path, _path);

  mz_zip_file *file_info = nullptr;
  err = mz_zip_reader_entry_get_info(native_handle, &file_info);
  if (err != MZ_OK) utils::error("Could not get entry info '{}' from archive '{}'", path, _path);

  mem.resize(file_info->uncompressed_size, 0);
  err = mz_zip_reader_entry_read(native_handle, mem.data(), mem.size());
  if (err != MZ_OK) utils::error("Could not read entry data '{}' from archive '{}'", path, _path);
}

void zip_module::load_binary(const std::string &path, std::vector<char> &mem) const {
  int32_t err = 0;
  err = mz_zip_reader_locate_entry(native_handle, path.c_str(), 0);
  if (err != MZ_OK) utils::error("Could not get entry '{}' from archive '{}'", path, _path);

  err = mz_zip_reader_entry_open(native_handle);
  if (err != MZ_OK) utils::error("Could not open entry '{}' from archive '{}'", path, _path);

  mz_zip_file *file_info = nullptr;
  err = mz_zip_reader_entry_get_info(native_handle, &file_info);
  if (err != MZ_OK) utils::error("Could not get entry info '{}' from archive '{}'", path, _path);

  mem.resize(file_info->uncompressed_size, 0);
  err = mz_zip_reader_entry_read(native_handle, mem.data(), mem.size());
  if (err != MZ_OK) utils::error("Could not read entry data '{}' from archive '{}'", path, _path);
}

void zip_module::load_text(const std::string &path, std::string &mem) const {
  int32_t err = 0;
  err = mz_zip_reader_locate_entry(native_handle, path.c_str(), 0);
  if (err != MZ_OK) utils::error("Could not get entry '{}' from archive '{}'", path, _path);

  err = mz_zip_reader_entry_open(native_handle);
  if (err != MZ_OK) utils::error("Could not open entry '{}' from archive '{}'", path, _path);

  mz_zip_file *file_info = nullptr;
  err = mz_zip_reader_entry_get_info(native_handle, &file_info);
  if (err != MZ_OK) utils::error("Could not get entry info '{}' from archive '{}'", path, _path);

  mem.resize(file_info->uncompressed_size, 0);
  err = mz_zip_reader_entry_read(native_handle, mem.data(), mem.size());
  if (err != MZ_OK) utils::error("Could not read entry data '{}' from archive '{}'", path, _path);
}
}
}