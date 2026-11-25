#include "devils_engine/utils/log.h"
#include "devils_engine/utils/print.h"
#include "devils_engine/utils/core.h"

#include <algorithm>
#include <limits>
#include "resource_system.h"
#include "zip_module.h"

#include <miniz.h>

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

// ZIP читает miniz: методы store и deflate. Архив со сжатием другим методом (zstd, bzip2, lzma)
// открывается, но такая запись отказывает при чтении с названием метода. Модули для раздачи пакуются
// без сжатия (store, стабильный порядок файлов): содержимое и так сжато (KTX2, opus), а Steam сжимает
// и раздаёт обновления сам.
namespace {
mz_zip_archive* archive_of(void* handle) noexcept {
  return static_cast<mz_zip_archive*>(handle);
}
const char* zip_error(mz_zip_archive* zip) noexcept {
  return mz_zip_get_error_string(mz_zip_get_last_error(zip));
}
} // namespace

void zip_module::open() {
  if (native_handle != nullptr) utils::error{}("Archive '{}' is already open", _path);
  auto* zip = new mz_zip_archive{};
  if (!mz_zip_reader_init_file(zip, _path.c_str(), 0)) {
    const std::string reason = zip_error(zip);
    delete zip;
    utils::error{}("Could not open archive '{}': {}", _path, reason);
  }
  native_handle = zip;
}
void zip_module::close() {
  if (native_handle == nullptr) {
    return;
  }
  auto* zip = archive_of(native_handle);
  native_handle = nullptr;
  const bool ok = mz_zip_reader_end(zip);
  const std::string reason = ok ? std::string() : zip_error(zip);
  delete zip;
  if (!ok) {
    utils::error{}("Could not close archive '{}': {}", _path, reason);
  }
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

void zip_module::resources_list(std::vector<resource_candidate>& out, const uint32_t module_priority) const {
  auto* zip = archive_of(native_handle);
  if (zip == nullptr) utils::error{}("Archive '{}' is not open while listing it", _path);
  const mz_uint count = mz_zip_reader_get_num_files(zip);
  for (mz_uint index = 0; index < count; ++index) {
    if (mz_zip_reader_is_file_a_directory(zip, index)) {
      continue;
    }
    mz_zip_archive_file_stat info{};
    if (!mz_zip_reader_file_stat(zip, index, &info)) {
      utils::error{}("Could not get entry {} from archive '{}': {}", index, _path, zip_error(zip));
    }

    std::string file_path = info.m_filename;
    const auto [id, name, ext] = parse_path(file_path);
    if (name == "." || name == "..") {
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
      static_cast<size_t>(info.m_uncomp_size),
      module_priority};

    if (ext == "tavl") {
      std::string content;
      load_text(candidate.path, content);
      if (append_tavl_list_candidates(out, candidate, content)) {
        continue;
      }
    }

    out.push_back(std::move(candidate));
  }
}

namespace {

// Одна проверенная граница для всех контейнеров: метод сжатия, объявленный размер, CRC (miniz
// сверяет его при распаковке).
template <typename Container>
void read_entry(void* handle, const std::string& archive, const std::string& path, Container& output) {
  auto* zip = archive_of(handle);
  if (zip == nullptr) utils::error{}("Archive '{}' is not open while reading entry '{}'", archive, path);
  const int index = mz_zip_reader_locate_file(zip, path.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
  if (index < 0) utils::error{}("Could not locate entry '{}' in archive '{}': {}", path, archive, zip_error(zip));
  mz_zip_archive_file_stat info{};
  if (!mz_zip_reader_file_stat(zip, mz_uint(index), &info)) {
    utils::error{}("Could not inspect entry '{}' in archive '{}': {}", path, archive, zip_error(zip));
  }
  if (info.m_method != 0 && info.m_method != MZ_DEFLATED) {
    utils::error{}("Entry '{}' in archive '{}' is compressed with ZIP method {}; only store (0) and deflate (8) are read",
                   path, archive, info.m_method);
  }
  if (info.m_is_encrypted) utils::error{}("Entry '{}' in archive '{}' is encrypted", path, archive);
  if (info.m_uncomp_size > output.max_size()) {
    utils::error{}("Entry '{}' in archive '{}' declares {} bytes, container limit {}", path, archive, info.m_uncomp_size, output.max_size());
  }
  output.resize(static_cast<size_t>(info.m_uncomp_size));
  if (output.empty()) {
    return;
  }
  if (!mz_zip_reader_extract_to_mem(zip, mz_uint(index), output.data(), output.size(), 0)) {
    utils::error{}("Could not read entry '{}' in archive '{}': {}", path, archive, zip_error(zip));
  }
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
