#ifndef DEVILS_ENGINE_DEMIURG_MODULE_INTERFACE_H
#define DEVILS_ENGINE_DEMIURG_MODULE_INTERFACE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "resource_manifest.h"

namespace devils_engine {
namespace demiurg {
class resource_interface;
class resource_system;

// название, размер и проч полезное
class module_interface {
public:
  // нужен чтобы найти подходящий тип объекта
  module_interface(std::string path) noexcept;
  virtual ~module_interface() noexcept = default;

  std::string_view path() const noexcept;

  virtual void open() = 0;
  virtual void close() = 0;
  virtual bool is_openned() const = 0;
  virtual void resources_list(std::vector<resource_candidate>& out, uint32_t module_priority) const = 0;
  // может быть потребуется индекс, нет индекс не потребуется
  // нужно заменить на обычный стринг, в минизипе нужно указать c_str
  // в этой строке нужно отправлять относительный путь до файла, но с именем файла и расширением
  virtual void load_binary(const std::string& path, std::vector<char>& mem) const = 0;
  virtual void load_binary(const std::string& path, std::vector<uint8_t>& mem) const = 0;
  virtual void load_text(const std::string& path, std::string& mem) const = 0;

  virtual std::vector<uint8_t> load_binary(const std::string& path) const;
  virtual std::string load_text(const std::string& path) const;

protected:
  //system *sys; // указать путь тут? имеет смысл
  std::string _path; // если папка то обязательно нужно добавить '/' на конце
};
} // namespace demiurg
} // namespace devils_engine

#endif
