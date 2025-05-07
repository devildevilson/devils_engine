#include "shader_crafter.h"

#include <memory>
#include <shaderc/shaderc.hpp>
#include "demiurg/resource_system.h"
#include "glsl_source_file.h"

#include "utils/core.h"

namespace devils_engine {
namespace painter {

class simple_shader_includer : public shaderc::CompileOptions::IncluderInterface {
public:
  simple_shader_includer(const demiurg::resource_system* sys) : _sys(sys) {}

  shaderc_include_result* GetInclude(
    const char* requested_source, // запрашиваемый файл
    shaderc_include_type type,
    const char* requesting_source, // файл который запрашивает
    size_t include_depth
  ) override {
    // со стороны шейдера к нам приходит запрос на включение файлов
    // скорее всего мы все пути будем обрабатывать одинаково
    // откуда берет инклюдер пути? из демиурга

    // нужно отдать shaderc_include_result
    // а в него положить имя файла и текст файла

    const auto file_name = std::string_view(requested_source);

    utils::println("requested_source", file_name, "requesting_source", requesting_source, "include_depth", include_depth);

    // + у нас могут быть несколько уникальных include 
    // например core_structures, укажем их типа #include <core>
    // их надо каким то образом положить в программу как текст

    std::vector<glsl_source_file*> files;
    files.reserve(2);

    // я всегда должен возвращать валидную память
    auto result = new shaderc_include_result;
    memset(result, 0, sizeof(shaderc_include_result));

    // мы указываем поиск во всех системах "добавления" исходного кода
    // предполагается что пользователь знает точный путь до файла и должен его указать
    // + даже при этом удобно искать именно список
    const size_t count = _sys->find<glsl_source_file>(file_name, files);
    if (count > 1) return result;
    if (count == 0) return result;

    auto file = files[0];

    // чтобы вылетела ошибка типа не найден файл source_name должен быть пустым
    result->source_name = requested_source;
    result->source_name_length = strlen(requested_source);
    result->content = file->memory.data();
    result->content_length = file->memory.size();

    return result;
  }

  void ReleaseInclude(shaderc_include_result* data) override {
    delete data;
  }

private:
  const demiurg::resource_system* _sys;
};

shader_crafter::shader_crafter(const demiurg::resource_system* sys) : _sys(sys), _opt(true), _type(0), _err_type(shaderc_compilation_status_success) {}

// здесь бы мы хотели принять на вход текст шейдера
// и получить на выход бинарник готовый к употреблению
// при этом: должны работать include и кастомные define
//

void shader_crafter::add_definition(std::string name, std::string value) {
  _definitions.push_back(std::make_pair(std::move(name), std::move(value)));
}

void shader_crafter::set_optimization(const bool opt) {
  _opt = opt;
}

void shader_crafter::set_shader_type(const uint32_t type) {
  _type = type;
}

void shader_crafter::set_shader_entry_point(std::string entry_point) {
  _entry_point = std::move(entry_point);
}

std::vector<uint32_t> shader_crafter::compile(const std::string &source_name, const std::string &source) {
  shaderc::Compiler compiler;
  shaderc::CompileOptions options;

  for (const auto &[name, value] : _definitions) {
    options.AddMacroDefinition(name, value);
  }

  options.SetIncluder(std::make_unique<simple_shader_includer>(_sys));
  options.SetTargetEnvironment(shaderc_target_env_vulkan, 0);
  //options.SetTargetSpirv(shaderc_spirv_version_1_6);
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  if (_opt)
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

  const auto kind = static_cast<shaderc_shader_kind>(_type);
  const auto preprocess_result = compiler.PreprocessGlsl(source, kind, source_name.c_str(), options);
  if (preprocess_result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = preprocess_result.GetCompilationStatus();
    _err = preprocess_result.GetErrorMessage();
    return {};
  }

  const auto preprocessed_source = std::string{preprocess_result.cbegin(), preprocess_result.cend()};
  utils::println("preprocessed_source", preprocessed_source);

  const auto result = compiler.CompileGlslToSpv(preprocessed_source, kind, source_name.c_str(), _entry_point.c_str(), options);
  if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = result.GetCompilationStatus();
    _err = result.GetErrorMessage();
    return {};
  }

  return std::vector<uint32_t>{result.cbegin(), result.cend()};
}

// было бы неплохо схранить статус ошибки где нибудь
uint32_t shader_crafter::err_type() const { return _err_type; }
const std::string& shader_crafter::err_msg() const { return _err; }

}
}