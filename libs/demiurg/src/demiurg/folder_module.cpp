#include "folder_module.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/core.h"
#include "resource_system.h"
#include <filesystem>
namespace fs = std::filesystem;

namespace devils_engine {
namespace demiurg {

  static void parse_path(
    const std::string &path, 
    const std::string_view &root_path,
    std::string_view &file_name, 
    std::string_view &ext,
    std::string_view &id
  ) {
    std::string_view full_path = path;
    utils_assertf(full_path.find(root_path) == 0, "Path to resource must have root folder part. Current path: {}", path);
    full_path = full_path.substr(root_path.size()+1);

    /*const size_t first_slash = full_path.find('/');
    module_name = first_slash != std::string_view::npos ? full_path.substr(0, first_slash) : "";
    const size_t last_slash_index = full_path.rfind('/');
    file_name = last_slash_index != std::string_view::npos ? full_path.substr(last_slash_index) : full_path;*/
    file_name = full_path;
    if (file_name == "." || file_name == "..") return;

    const size_t dot_index = file_name.rfind('.');
    ext = dot_index != 0 && dot_index != std::string_view::npos ? file_name.substr(dot_index+1) : "";
    //const size_t module_size = module_name == "" ? 0 : module_name.size()+1;
    const size_t ext_size = ext == "" ? 0 : ext.size()+1;
    id = full_path.substr(0, full_path.size() - ext_size);
  }

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

  folder_module::folder_module(std::string root) noexcept : module_interface(std::move(root))
  {
      if (_path[_path.size() - 1] != '/') _path += '/';
      const size_t index = _path.rfind('/', _path.size()-1);
      module_name = std::string_view(_path).substr(index);
      if (module_name.back() == '/') module_name = module_name.substr(0, module_name.size()-1);
  }

  std::string_view folder_module::name() const { return module_name; }

  void folder_module::open() {}
  void folder_module::close() {}
  bool folder_module::is_openned() const { return true; }

  //void folder_module::resources_list(std::vector<resource_interface*> &arr) const {
  //  for (const auto &entry : fs::recursive_directory_iterator(_path)) {
  //    if (!entry.is_regular_file()) continue;
  //    std::string entry_path = entry.path().generic_string();
  //    auto file_path = entry_path.substr(_path.size());

  //    // module_name мы теперь знаем и тут он нам не нужен
  //    //std::string_view file_name, ext, id;
  //    //parse_path(entry_path, _path, file_name, ext, id);
  //    const auto [ id, name, ext ] = parse_path(file_path);
  //    if (name == "." || name == "..") continue;

  //    auto t = sys->find_type(id, ext);

  //    if (t == nullptr) {
  //      utils::warn("Could not find proper resource type for file '{}'. Skip", entry_path);
  //      continue;
  //    }

  //    auto res = t->create();
  //    // указание пути тоже нужно переделывать
  //    // наверное просто самостоятельно положить данные какие нужны
  //    //res->set_path(entry_path, _root);
  //    // скорее всего тут нужно указать только путь а id и ext вывести из него на месте во избежание проблем
  //    res->set(std::move(file_path), module_name, id, ext);
  //    res->module = this;
  //    res->raw_size = entry.file_size();
  //    arr.push_back(res);
  //  }
  //}

  // полный путь? тут как будто полный путь передать неполучится, придется собирать строку
  // тут вообще не нужно передавать полный путь !!! ТОЛЬКО относительный
  // когда делаем реквайр наверное будем указывать вот так
  // local res = require("abc/def/asd")
  // local res = require("module1:abc/def/asd")
  // скорее всего реквайр просто закинет путь в поиск в демиурге и если какой то путь будет бредовый то не найдет ничего
  // так нам нужно еще сделать что то вроде require_list("abc/def/asd"), в нем указываем неполный путь
  // require_list вернет массив объектов и должен автоматом преобразовать их в нужные юзертипы

  void folder_module::resources_list(resource_system* s) const {
    for (const auto &entry : fs::recursive_directory_iterator(_path)) {
      if (!entry.is_regular_file()) continue;
      std::string entry_path = entry.path().generic_string();
      auto file_path = entry_path.substr(_path.size());

      // module_name мы теперь знаем и тут он нам не нужен
      const auto [ id, name, ext ] = parse_path(file_path);
      if (name == "." || name == "..") continue;

      auto res = s->create(id, ext);
      if (res == nullptr) {
        utils::warn("Could not find proper resource type for file '{}'. Skip", entry_path);
        continue;
      }

      res->set(std::move(file_path), module_name, id, ext);
      res->module = this;
      res->raw_size = entry.file_size();
    }
  }

  void folder_module::load_binary(const std::string &path, std::vector<uint8_t> &mem) const {
    mem = file_io::read<uint8_t>(_path+path);
  }
  
  void folder_module::load_binary(const std::string &path, std::vector<char> &mem) const {
    mem = file_io::read<char>(_path+path);
  }
  
  void folder_module::load_text(const std::string &path, std::string &mem) const {
    mem = file_io::read(_path+path);
  }

  std::vector<uint8_t> folder_module::load_binary(const std::string &path) const {
    return file_io::read<uint8_t>(_path+path);
  }
  
  std::string folder_module::load_text(const std::string &path) const {
    return file_io::read(_path+path);
  }

}
}