#ifndef DEVILS_ENGINE_PAINTER_SHADER_CRAFTER_H
#define DEVILS_ENGINE_PAINTER_SHADER_CRAFTER_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace devils_engine {
namespace demiurg {
class resource_system;
}

namespace painter {
class shader_crafter {
public:
  shader_crafter(const demiurg::resource_system* sys);

  // здесь бы мы хотели принять на вход текст шейдера
  // и получить на выход бинарник готовый к употреблению
  // при этом: должны работать include и кастомные define
  // 

  void add_definition(std::string name, std::string value);
  void set_optimization(const bool opt);
  void set_shader_type(const uint32_t type);
  void set_shader_entry_point(std::string entry_point);
  std::vector<uint32_t> compile(const std::string &source_name, const std::string &source);
  // было бы неплохо схранить статус ошибки где нибудь
  uint32_t err_type() const;
  const std::string & err_msg() const;
private:
  const demiurg::resource_system* _sys;
  bool _opt;
  uint32_t _type;
  uint32_t _err_type;
  std::string _entry_point;
  std::vector<std::pair<std::string, std::string>> _definitions;
  std::string _err;
};
}
}

#endif