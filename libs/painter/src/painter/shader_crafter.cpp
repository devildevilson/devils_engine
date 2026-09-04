#include <cstring>
#include <deque>
#include <memory>
#include <string_view>

#include <shaderc/shaderc.hpp>

#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/painter/bindings_shared_include_text.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "glsl_source_file.h"
#include "shader_crafter.h"

namespace devils_engine {
namespace painter {

namespace {

constexpr std::string_view utils_shared_include_name = "utils/shared.h";
constexpr std::string_view legacy_bindings_shared_include_name = "bindings/shared.h";

shaderc_include_result* make_empty_include_result() {
  auto* result = new shaderc_include_result;
  std::memset(result, 0, sizeof(shaderc_include_result));
  return result;
}

} // namespace

class simple_shader_includer : public shaderc::CompileOptions::IncluderInterface {
public:
  // include_root != пусто => файлы ищутся ещё и на диске относительно этого корня. Нужно лабораториям,
  // которые читают шейдеры прямо из дерева исходников (без demiurg-реестра): без этого общий файл с
  // объявлениями записей невозможен, а дублировать layout по пяти шейдерам — гарантированный баг,
  // потому что рассинхрон виден только по кривой картинке.
  simple_shader_includer(const demiurg::resource_system* sys, std::string include_root)
    : _sys(sys), _include_root(std::move(include_root)) {}

  shaderc_include_result* GetInclude(
    const char* requested_source, // запрашиваемый файл
    shaderc_include_type,
    const char*, // файл который запрашивает
    size_t) override {
    const auto file_name = std::string_view(requested_source);

    if (file_name == utils_shared_include_name || file_name == legacy_bindings_shared_include_name) {
      auto* result = make_empty_include_result();
      result->source_name = requested_source;
      result->source_name_length = std::strlen(requested_source);
      result->content = bindings_shared_include_text.data();
      result->content_length = bindings_shared_include_text.size();
      return result;
    }

    std::vector<glsl_source_file*> files;
    files.reserve(2);

    // я всегда должен возвращать валидную память
    auto result = make_empty_include_result();

    if (!_include_root.empty()) {
      const auto full_path = _include_root + std::string(file_name);
      if (file_io::exists(full_path)) {
        _contents.push_back(file_io::read(full_path));
        const auto& content = _contents.back();
        result->source_name = requested_source;
        result->source_name_length = std::strlen(requested_source);
        result->content = content.data();
        result->content_length = content.size();
        return result;
      }
    }

    if (_sys == nullptr) {
      return result;
    }

    // мы указываем поиск во всех системах "добавления" исходного кода
    // предполагается что пользователь знает точный путь до файла и должен его указать
    // + даже при этом удобно искать именно список
    const size_t count = _sys->find<glsl_source_file>(file_name, files);
    if (count > 1) {
      return result;
    }
    if (count == 0) {
      return result;
    }

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
  std::string _include_root;
  // Содержимое обязано жить до конца компиляции: shaderc держит только указатель.
  std::deque<std::string> _contents;
};

shader_crafter::shader_crafter(const demiurg::resource_system* sys) : _sys(sys), _opt(true), _debug_info(false), _type(0), _err_type(shaderc_compilation_status_success) {}

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

void shader_crafter::set_include_root(std::string root) {
  _include_root = std::move(root);
}

void shader_crafter::set_debug_info(const bool enable) {
  _debug_info = enable;
}

void shader_crafter::set_shader_type(const uint32_t type) {
  _type = type;
}

void shader_crafter::set_shader_entry_point(std::string entry_point) {
  _entry_point = std::move(entry_point);
}

std::vector<uint32_t> shader_crafter::compile(const std::string& source_name, const std::string& source) {
  // КОМПИЛЯТОР ЖИВЁТ ПО ОДНОМУ НА ПОТОК, а не создаётся заново на каждый вызов, и это ИЗМЕРЕННАЯ
  // разница, а не гигиена.
  //
  // Конструктор `shaderc::Compiler` поднимает внутреннее состояние glslang, и стоит это около 90 мс
  // — независимо от того, что компилируется. Пока компилятор создавался внутри этой функции, цену
  // платил КАЖДЫЙ шейдер движка: замер на вычислительном контексте показал 100 мс на программу, из
  // которых сам шейдер занимал 1–2.3 мс (без оптимизатора 1.1 мс). То есть 98% времени компиляции
  // уходило на то, чтобы завести компилятор и тут же его выбросить.
  //
  // Почему `thread_local`, а не поле `shader_crafter`: крафтер создаётся НА ОДИН вызов почти у всех
  // потребителей (`glsl_source_file::prepare_spirv`, вычислительный контекст), поэтому поле не помогло
  // бы им ничем — платили бы столько же. А состояние glslang по своей природе процессное, и один
  // компилятор на поток — честная его модель. Один объект `shaderc::Compiler` не потокобезопасен,
  // отсюда именно `thread_local`, а не просто `static`.
  static thread_local shaderc::Compiler compiler;
  shaderc::CompileOptions options;

  for (const auto& [name, value] : _definitions) {
    options.AddMacroDefinition(name, value);
  }

  options.SetTargetEnvironment(shaderc_target_env_vulkan, 0);
  //options.SetTargetSpirv(shaderc_spirv_version_1_6);
  options.SetTargetSpirv(shaderc_spirv_version_1_0);
  if (_opt) {
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
  }
  if (_debug_info) {
    options.SetGenerateDebugInfo();
  }

  options.SetIncluder(std::make_unique<simple_shader_includer>(_sys, _include_root));
  const auto kind = static_cast<shaderc_shader_kind>(_type);
  const auto preprocess_result = compiler.PreprocessGlsl(source, kind, source_name.c_str(), options);
  if (preprocess_result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = preprocess_result.GetCompilationStatus();
    _err = preprocess_result.GetErrorMessage();
    return {};
  }

  const auto preprocessed_source = std::string{preprocess_result.cbegin(), preprocess_result.cend()};

  const auto result = compiler.CompileGlslToSpv(preprocessed_source, kind, source_name.c_str(), _entry_point.c_str(), options);
  if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = result.GetCompilationStatus();
    _err = result.GetErrorMessage();
    return {};
  }

  return std::vector<uint32_t>{result.cbegin(), result.cend()};
}

// было бы неплохо схранить статус ошибки где нибудь
uint32_t shader_crafter::err_type() const {
  return _err_type;
}
const std::string& shader_crafter::err_msg() const {
  return _err;
}

} // namespace painter
} // namespace devils_engine
