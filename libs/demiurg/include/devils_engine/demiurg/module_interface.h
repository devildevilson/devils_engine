#ifndef DEVILS_ENGINE_DEMIURG_MODULE_INTERFACE_H
#define DEVILS_ENGINE_DEMIURG_MODULE_INTERFACE_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace devils_engine {
namespace demiurg {
  class resource_interface;
  class resource_system;

  // название, размер и проч полезное
  class module_interface {
  public:
    // нужен чтобы найти подходящий тип объекта
    inline module_interface(std::string path) noexcept : _path(std::move(path)) {}
    virtual ~module_interface() noexcept = default;

    inline std::string_view path() const { return _path; }

    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool is_openned() const = 0;
    //virtual void resources_list(std::vector<resource_interface*> &arr) const = 0;
    virtual void resources_list(resource_system* system) const = 0;
    // может быть потребуется индекс, нет индекс не потребуется
    // нужно заменить на обычный стринг, в минизипе нужно указать c_str
    // в этой строке нужно отправлять относительный путь до файла, но с именем файла и расширением
    virtual void load_binary(const std::string &path, std::vector<char> &mem) const = 0;
    virtual void load_binary(const std::string &path, std::vector<uint8_t> &mem) const = 0;
    virtual void load_text(const std::string &path, std::string &mem) const = 0;

    virtual std::vector<uint8_t> load_binary(const std::string &path) const {
      std::vector<uint8_t> mem;
      load_binary(path, mem);
      return mem;
    }

    virtual std::string load_text(const std::string &path) const {
      std::string mem;
      load_text(path, mem);
      return mem;
    }
  protected:
    //system *sys; // указать путь тут? имеет смысл
    std::string _path; // если папка то обязательно нужно добавить '/' на конце
  };
}
}

#endif