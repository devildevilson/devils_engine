#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <deque>
#include <memory>
#include <string_view>

#include <shaderc/shaderc.hpp>

#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/painter/bindings_shared_include_text.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"
#include "devils_engine/utils/hash.h"
#include "devils_engine/utils/type_traits.h"
#include "glsl_source_file.h"
#include "shader_compiler_state.h"
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

// РАЗРЕШЕНИЕ `#include` ВЫНЕСЕНО ИЗ includer'а, потому что его зовут ДВОЕ: сам includer во время
// компиляции и проверка записи дискового кэша — той нужно узнать содержимое включённого файла, НЕ
// поднимая glslang. Один способ найти файл на оба случая: второй разъехался бы с первым молча, и
// кэш начал бы отвечать на изменившийся исходник старым SPIR-V. Текст КОПИРУЕТСЯ: включаемые файлы
// невелики, а общий на два случая владелец жизни строки был бы сложнее, чем эта копия.
bool resolve_include_text(const demiurg::resource_system* sys,
                          const std::string& include_root,
                          const std::string_view& file_name,
                          std::string& out) {
  if (file_name == utils_shared_include_name || file_name == legacy_bindings_shared_include_name) {
    out.assign(bindings_shared_include_text);
    return true;
  }

  if (!include_root.empty()) {
    const auto full_path = include_root + std::string(file_name);
    if (file_io::exists(full_path)) {
      out = file_io::read(full_path);
      return true;
    }
  }

  if (sys == nullptr) {
    return false;
  }

  std::vector<glsl_source_file*> files;
  files.reserve(2);
  if (sys->find<glsl_source_file>(file_name, files) != 1) {
    return false;
  }
  out = files[0]->memory;
  return true;
}

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

    // я всегда должен возвращать валидную память
    auto* result = make_empty_include_result();

    std::string text;
    if (!resolve_include_text(_sys, _include_root, file_name, text)) {
      // чтобы вылетела ошибка типа не найден файл source_name должен быть пустым
      return result;
    }

    // Содержимое обязано жить до конца компиляции: shaderc держит только указатель.
    _contents.push_back(std::move(text));
    const auto& content = _contents.back();
    // ЧТО ИМЕННО ПОДСТАВИЛОСЬ — записывается здесь, потому что только здесь это и известно. Список
    // уезжает в запись дискового кэша: ключ по одному тексту шейдера правку включённого файла не
    // заметил бы.
    _dependencies.push_back(shader_dependency{std::string(file_name), utils::murmur_hash64A(content)});

    result->source_name = requested_source;
    result->source_name_length = std::strlen(requested_source);
    result->content = content.data();
    result->content_length = content.size();
    return result;
  }

  void ReleaseInclude(shaderc_include_result* data) override {
    delete data;
  }

  const std::vector<shader_dependency>& dependencies() const noexcept { return _dependencies; }

private:
  const demiurg::resource_system* _sys;
  std::string _include_root;
  std::deque<std::string> _contents;
  std::vector<shader_dependency> _dependencies;
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

namespace {

// Ключ записи кэша: ВСЁ, от чего зависит выход. Текст шейдера, род стадии, точка входа, дефайны,
// оптимизатор, debug info и корень включений — стоит забыть одно, и кэш ответит чужим SPIR-V, а
// понять это по поведению нельзя (шейдер просто считает не то). Включённые файлы в ключ не входят
// НАРОЧНО: их состав становится известен только после препроцессинга, то есть после подъёма glslang,
// — а именно его тёплый прогон и обязан не платить. Вместо этого они проверяются СОДЕРЖИМЫМ при
// чтении записи.
uint64_t compilation_key(const std::string& source_name,
                         const std::string& source,
                         const uint32_t type,
                         const std::string& entry_point,
                         const std::string& include_root,
                         const bool opt,
                         const bool debug_info,
                         const std::span<const std::pair<std::string, std::string>>& definitions) {
  uint64_t key = utils::murmur_hash64A(source);
  key = utils::hash_combine(key, utils::murmur_hash64A(source_name));
  key = utils::hash_combine(key, utils::murmur_hash64A(entry_point));
  key = utils::hash_combine(key, utils::murmur_hash64A(include_root));
  key = utils::hash_combine(key, uint64_t(type));
  key = utils::hash_combine(key, uint64_t(opt) | (uint64_t(debug_info) << 1));
  for (const auto& [name, value] : definitions) {
    key = utils::hash_combine(key, utils::murmur_hash64A(name));
    key = utils::hash_combine(key, utils::murmur_hash64A(value));
  }
  return key;
}

std::string cache_entry_path(const std::string& directory, const uint64_t key) {
  return std::format("{}/{:016x}.despv", directory, key);
}

// Формат записи выбран самым скучным из возможных: он читается и пишется одной функцией каждая, и
// его версия стоит в заголовке — сменившийся формат обязан быть ПРОМАХОМ, а не мусором на входе.
constexpr uint32_t cache_format_version = 1;

struct cache_entry {
  std::vector<shader_dependency> dependencies;
  std::vector<uint32_t> spirv;
};

bool read_u32(const std::span<const uint8_t>& bytes, size_t& at, uint32_t& out) {
  if (at + sizeof(out) > bytes.size()) return false;
  std::memcpy(&out, bytes.data() + at, sizeof(out));
  at += sizeof(out);
  return true;
}

bool read_u64(const std::span<const uint8_t>& bytes, size_t& at, uint64_t& out) {
  if (at + sizeof(out) > bytes.size()) return false;
  std::memcpy(&out, bytes.data() + at, sizeof(out));
  at += sizeof(out);
  return true;
}

void write_u32(std::vector<uint8_t>& bytes, const uint32_t value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), p, p + sizeof(value));
}

void write_u64(std::vector<uint8_t>& bytes, const uint64_t value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), p, p + sizeof(value));
}

bool parse_cache_entry(const std::span<const uint8_t>& bytes, const uint64_t key, cache_entry& out) {
  size_t at = 0;
  uint32_t version = 0;
  uint64_t stored_key = 0;
  uint32_t dependency_count = 0;
  if (!read_u32(bytes, at, version) || version != cache_format_version) return false;
  // Ключ лежит и ВНУТРИ записи, а не только в имени файла: имя может совпасть у двух ключей только
  // при столкновении хеша, и тогда чужая запись обязана быть отвергнута, а не исполнена.
  if (!read_u64(bytes, at, stored_key) || stored_key != key) return false;
  if (!read_u32(bytes, at, dependency_count)) return false;

  out.dependencies.reserve(dependency_count);
  for (uint32_t i = 0; i < dependency_count; ++i) {
    uint32_t name_length = 0;
    uint64_t content_hash = 0;
    if (!read_u32(bytes, at, name_length)) return false;
    if (at + name_length > bytes.size()) return false;
    std::string name(reinterpret_cast<const char*>(bytes.data() + at), name_length);
    at += name_length;
    if (!read_u64(bytes, at, content_hash)) return false;
    out.dependencies.push_back(shader_dependency{std::move(name), content_hash});
  }

  uint32_t word_count = 0;
  if (!read_u32(bytes, at, word_count)) return false;
  if (at + size_t(word_count) * sizeof(uint32_t) > bytes.size()) return false;
  out.spirv.resize(word_count);
  std::memcpy(out.spirv.data(), bytes.data() + at, size_t(word_count) * sizeof(uint32_t));
  return word_count != 0;
}

} // namespace

std::vector<uint32_t> shader_crafter::compile(shader_compiler& compiler,
                                              const std::string& source_name,
                                              const std::string& source) {
  auto& own = *compiler.state_;
  const auto key = compilation_key(source_name, source, _type, _entry_point, _include_root, _opt,
                                   _debug_info, _definitions);

  // 1. ДИСКОВЫЙ КЭШ. Читается ДО того, как поднято состояние glslang: в этом вся его ценность —
  //    конструктор `shaderc::Compiler` стоит около 90 мс, и полностью тёплый прогон не платит их
  //    вовсе. Экономия на самой компиляции (1–2.3 мс на шейдер) здесь вторична.
  if (!own.cache_dir.empty()) {
    const auto path = cache_entry_path(own.cache_dir, key);
    if (file_io::exists(path)) {
      const auto bytes = file_io::read<uint8_t>(path);
      cache_entry entry;
      if (parse_cache_entry(bytes, key, entry)) {
        // ЗАПИСЬ ПРОВЕРЯЕТСЯ ПО СОДЕРЖИМОМУ ВКЛЮЧЁННЫХ ФАЙЛОВ, а не по времени правки: правка
        // включённого файла обязана быть промахом, иначе движок исполнит не тот шейдер, который
        // написан, — и по картинке этого не увидеть. Файлы читаются, но glslang не поднимается.
        bool current = true;
        std::string text;
        for (const auto& dependency : entry.dependencies) {
          if (!resolve_include_text(_sys, _include_root, dependency.name, text) ||
              utils::murmur_hash64A(text) != dependency.content_hash) {
            current = false;
            break;
          }
        }
        if (current) {
          own.hits += 1;
          _err.clear();
          _err_type = shaderc_compilation_status_success;
          return std::move(entry.spirv);
        }
      }
    }
  }

  // 2. НАСТОЯЩАЯ КОМПИЛЯЦИЯ. `own.glslang()` поднимает состояние при ПЕРВОМ обращении и держит его
  //    столько, сколько живёт переданный компилятор: цена в 90 мс платится один раз на пачку, а не
  //    на шейдер (98% времени компиляции уходило именно на неё).
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

  auto includer = std::make_unique<simple_shader_includer>(_sys, _include_root);
  // Указатель берётся ДО передачи владения: список подставленных файлов нужен уже после компиляции,
  // а сам includer к тому моменту принадлежит опциям.
  const auto* resolved = includer.get();
  options.SetIncluder(std::move(includer));

  auto& glslang = own.glslang();
  own.compilations += 1;

  const auto kind = static_cast<shaderc_shader_kind>(_type);
  const auto preprocess_result = glslang.PreprocessGlsl(source, kind, source_name.c_str(), options);
  if (preprocess_result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = preprocess_result.GetCompilationStatus();
    _err = preprocess_result.GetErrorMessage();
    return {};
  }

  const auto preprocessed_source = std::string{preprocess_result.cbegin(), preprocess_result.cend()};

  const auto result = glslang.CompileGlslToSpv(preprocessed_source, kind, source_name.c_str(), _entry_point.c_str(), options);
  if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
    _err_type = result.GetCompilationStatus();
    _err = result.GetErrorMessage();
    return {};
  }

  std::vector<uint32_t> spirv{result.cbegin(), result.cend()};

  // 3. ЗАПИСЬ В КЭШ. Неудача записи — НЕ ошибка компиляции: каталог, в который нельзя писать, обязан
  //    вести себя как отсутствие кэша. Иначе кэш, задуманный как ускорение, становится причиной
  //    отказа собрать шейдер.
  if (!own.cache_dir.empty() && !spirv.empty()) {
    std::vector<uint8_t> bytes;
    write_u32(bytes, cache_format_version);
    write_u64(bytes, key);
    const auto& dependencies = resolved->dependencies();
    write_u32(bytes, uint32_t(dependencies.size()));
    for (const auto& dependency : dependencies) {
      write_u32(bytes, uint32_t(dependency.name.size()));
      bytes.insert(bytes.end(), dependency.name.begin(), dependency.name.end());
      write_u64(bytes, dependency.content_hash);
    }
    write_u32(bytes, uint32_t(spirv.size()));
    const auto* words = reinterpret_cast<const uint8_t*>(spirv.data());
    bytes.insert(bytes.end(), words, words + spirv.size() * sizeof(uint32_t));

    // НЕУДАЧА ЗАПИСИ НЕ ЯВЛЯЕТСЯ НЕУДАЧЕЙ КОМПИЛЯЦИИ, и поэтому она ловится здесь: `create_directory`
    // на непригодном пути БРОСАЕТ, а кэш, задуманный как ускорение, не вправе стать причиной того,
    // что шейдер не собрался. Жалоба при этом одна на компилятор, а не на шейдер: каталог не
    // исправится сам, и повторять её на каждый шейдер значило бы заглушить весь остальной вывод.
    try {
      if (!file_io::exists(own.cache_dir)) {
        file_io::create_directory(own.cache_dir);
      }
      if (!file_io::write(std::span<const uint8_t>(bytes), cache_entry_path(own.cache_dir, key))) {
        throw std::runtime_error("write failed");
      }
    } catch (const std::exception& error) {
      if (!own.cache_complained) {
        own.cache_complained = true;
        utils::warn("Shader cache directory '{}' is not usable ({}); compiling every run",
                    own.cache_dir, error.what());
      }
    }
  }

  return spirv;
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
