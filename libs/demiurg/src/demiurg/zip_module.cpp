#include "devils_engine/utils/core.h"

#include <algorithm>
#include <limits>
#include "resource_system.h"
#include "zip_module.h"

//#include "utils/mz.h"
//#include "utils/mz_zip.h"
//#include "utils/mz_strm.h"
//#include "utils/mz_zip_rw.h"
#include "mz.h"
#include "mz_strm.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

namespace devils_engine {
namespace demiurg {
zip_module::zip_module(std::string root, std::string name) noexcept
  : module_interface(std::move(root)), native_handle(nullptr), module_name(std::move(name)) {}

zip_module::~zip_module() noexcept {
  close();
}

std::string_view zip_module::name() const {
  return module_name;
}

void zip_module::open() {
  if (native_handle != nullptr) utils::error{}("Archive '{}' is already open", _path);
  native_handle = mz_zip_reader_create();
  const auto err = mz_zip_reader_open_file(native_handle, _path.c_str());
  if (err != MZ_OK) {
    mz_zip_reader_delete(&native_handle);
    utils::error{}("Could not open archive '{}'", _path);
  }
}
void zip_module::close() {
  if (native_handle == nullptr) {
    return;
  }
  const auto err = mz_zip_reader_close(native_handle);
  if (err != MZ_OK) {
    utils::error{}("Could not close archive '{}'", _path);
  }
  mz_zip_reader_delete(&native_handle);
  native_handle = nullptr;
}

bool zip_module::is_openned() const {
  return native_handle != nullptr;
}

// тут бы желательно взять путь, убрать часть пути папки и парсить чисто остаток
static std::tuple<std::string_view, std::string_view, std::string_view> parse_path(const std::string_view& path) {
  // раньше я брал file_name как оносительный путь от папки
  // теперь наверное проще сделать по другому
  const size_t last_slash = path.rfind('/');
  const size_t last_dot = path.rfind('.');
  // здесь не должна приходить папка, должно ли у файлов быть какое то разрешение в принципе?
  // да именно по разрешению мы понимаем что к чему относится
  const auto ext = last_dot != std::string_view::npos ? path.substr(last_dot + 1) : std::string_view();
  const auto id = path.substr(0, last_dot);
  const auto name = path.substr(last_slash + 1).substr(0, last_dot);
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
//    if (cur != MZ_OK) utils::error{}("Could not get entry from archive '{}'", _path);
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

void zip_module::resources_list(std::vector<resource_candidate>& out, const uint32_t module_priority) const {
  int32_t err = MZ_OK;
  err = mz_zip_reader_goto_first_entry(native_handle);
  while (err == MZ_OK) {
    const int32_t is_dir = mz_zip_reader_entry_is_dir(native_handle);

    if (is_dir == MZ_OK) {
      err = mz_zip_reader_goto_next_entry(native_handle);
      continue;
    }

    mz_zip_file* file_info = nullptr;
    auto cur = mz_zip_reader_entry_get_info(native_handle, &file_info);
    if (cur != MZ_OK) {
      utils::error{}("Could not get entry from archive '{}'", _path);
    }

    //utils::println(file_info->filename, file_info->uncompressed_size);
    std::string file_path = file_info->filename;

    const auto [id, name, ext] = parse_path(file_path);
    if (name == "." || name == "..") {
      err = mz_zip_reader_goto_next_entry(native_handle);
      continue;
    }
    std::string id_str(id);
    std::string ext_str(ext);

    resource_candidate candidate{
      std::move(file_path),
      std::move(id_str),
      std::move(ext_str),
      {},
      {},
      {},
      module_name,
      this,
      static_cast<size_t>(file_info->uncompressed_size),
      module_priority};

    if (ext == "tavl") {
      std::string content;
      load_text(candidate.path, content);
      if (append_tavl_list_candidates(out, candidate, content)) {
        err = mz_zip_reader_goto_next_entry(native_handle);
        continue;
      }
    }

    out.push_back(std::move(candidate));

    err = mz_zip_reader_goto_next_entry(native_handle);
  }
}

namespace {

// Minizip read возвращает число байт, а не MZ_OK. Одна проверенная граница для всех
// container overloads; close проверяет CRC, guard закрывает entry при критическом отказе.
template <typename Container>
void read_entry(void* handle, const std::string& archive, const std::string& path, Container& output) {
  if (handle == nullptr) utils::error{}("Archive '{}' is not open while reading entry '{}'", archive, path);
  auto result = mz_zip_reader_locate_entry(handle, path.c_str(), 0);
  if (result != MZ_OK) utils::error{}("Could not locate entry '{}' in archive '{}': minizip {}", path, archive, result);
  result = mz_zip_reader_entry_open(handle);
  if (result != MZ_OK) utils::error{}("Could not open entry '{}' in archive '{}': minizip {}", path, archive, result);
  struct entry_guard {
    void* handle;
    bool open = true;
    ~entry_guard() noexcept {
      if (open) mz_zip_reader_entry_close(handle);
    }
  } guard{handle};
  mz_zip_file* info = nullptr;
  result = mz_zip_reader_entry_get_info(handle, &info);
  if (result != MZ_OK || info == nullptr) {
    utils::error{}("Could not inspect entry '{}' in archive '{}': minizip {}", path, archive, result);
  }
  if (info->uncompressed_size < 0 || uint64_t(info->uncompressed_size) > output.max_size()) {
    utils::error{}("Entry '{}' in archive '{}' declares {} bytes, container limit {}", path, archive, info->uncompressed_size, output.max_size());
  }
  output.resize(static_cast<size_t>(info->uncompressed_size));
  size_t offset = 0;
  while (offset < output.size()) {
    const auto requested = static_cast<int32_t>(std::min(output.size() - offset, size_t(INT32_MAX)));
    const auto got = mz_zip_reader_entry_read(handle, output.data() + offset, requested);
    if (got < 0) utils::error{}("Could not read entry '{}' in archive '{}' at byte {}: minizip {}", path, archive, offset, got);
    if (got == 0) utils::error{}("Short read of entry '{}' in archive '{}': got {} of {} bytes", path, archive, offset, output.size());
    offset += size_t(got);
  }
  result = mz_zip_reader_entry_close(handle);
  guard.open = false;
  if (result != MZ_OK) utils::error{}("Could not finish entry '{}' in archive '{}' (CRC/close): minizip {}", path, archive, result);
}

} // namespace

void zip_module::load_binary(const std::string& path, std::vector<uint8_t>& mem) const {
  read_entry(native_handle, _path, path, mem);
}

void zip_module::load_binary(const std::string& path, std::vector<char>& mem) const {
  read_entry(native_handle, _path, path, mem);
}

void zip_module::load_text(const std::string& path, std::string& mem) const {
  read_entry(native_handle, _path, path, mem);
}
} // namespace demiurg
} // namespace devils_engine
