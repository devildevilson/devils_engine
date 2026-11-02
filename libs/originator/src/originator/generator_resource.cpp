#include "devils_engine/originator/generator_resource.h"

#include <algorithm>

#include "devils_engine/demiurg/module_interface.h"
#include "devils_engine/demiurg/resource_path.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/core.h"

// Шов между `originator` и `demiurg`: id ресурса -> текст документа -> собранный `generator_config`.
//
// ЯДРО ГЕНЕРАТОРА ЗНАЕТ ТОЛЬКО ТЕКСТ, и второй способ искать файлы здесь не заводится: ссылки внутри
// генератора разрешаются ОДНИМ переводом (`demiurg::absolute_resource_path`), тем же, каким это
// делает `require` в lua — путь с точки считается от папки точки входа, любой другой от корня модуля.
//
// ТЕКСТЫ СКРИПТОВ КОПИРУЮТСЯ В КОНФИГ, а не берутся из реестра по требованию: иначе id тела шага
// пришлось бы держать живым до конца генерации, то есть держать открытым реестр, а `script_host`
// принимает текст и ходить за файлами не должен.
//
// РАЗДЕЛИТЕЛЯ `//---` В ТОЧКЕ ВХОДА БЫТЬ НЕ ДОЛЖНО: demiurg разложил бы такой файл на суб-ресурсы
// `путь:имя`, и id без хвоста перестал бы существовать — ошибка выглядела бы как опечатка в пути.

namespace devils_engine {
namespace originator {

namespace {

// Текст ресурса по абсолютному id. Ресурс берётся и отпускается сразу: документы генератора малы, а
// держать их загруженными незачем — дальше живёт уже разобранное описание.
//
// `referrer` попадает в сообщение об ошибке, потому что «нет такого ресурса» без ответа на вопрос
// «кто его просил» отправляет искать опечатку по всему модулю.
std::string read_source(const demiurg::resource_system& resources, const std::string& id, const std::string_view& referrer) {
  auto* res = resources.get<generator_source>(id);
  if (res == nullptr) {
    // Отдельная подсказка про `//---`: файл с этим маркером demiurg раскладывает на суб-ресурсы, и
    // тогда id без хвоста `:имя` не существует вовсе. Ошибка выглядит как опечатка в пути, хотя файл
    // лежит на месте, поэтому подсказка стоит прямо здесь.
    utils::error{}("originator: generator resource '{}' (referenced by '{}') not found; "
                   "check that the file lives under the registered type segment and that it has no '//---' "
                   "separator, which would split it into ':name' sub-resources",
                   id, referrer);
  }

  res->ensure_text_loaded();
  std::string text = res->text;
  res->drop_text();
  if (text.empty()) {
    utils::error{}("originator: generator resource '{}' (referenced by '{}') is empty", id, referrer);
  }
  return text;
}

// Разрешает ссылку из точки входа, запоминает текст и отдаёт АБСОЛЮТНЫЙ id. Отдаёт именно id, а не
// текст: тексты лежат в векторе, который продолжает расти, и ссылка на его элемент пережила бы ровно
// до следующей ссылки. Один и тот же id, названный дважды (общая программа devils_script у двух
// шагов — обычное дело), читается один раз.
std::string acquire(generator_config& config, const demiurg::resource_system& resources,
                    const std::string_view& raw, const std::string_view& referrer) {
  auto id = demiurg::absolute_resource_path(config.entry_id, raw);
  if (id.empty()) {
    utils::error{}("originator: '{}' names an empty or escaping resource path '{}'", referrer, raw);
  }

  for (const auto& [known, text] : config.sources) {
    if (known == id) {
      return id;
    }
  }

  config.sources.emplace_back(id, read_source(resources, id, referrer));
  return id;
}

} // namespace

generator_source::generator_source() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true);
  set_flag(demiurg::resource_flags::binary, false);
}

void generator_source::ensure_text_loaded() {
  if (!text.empty()) {
    return;
  }

  if (is_list_entry()) {
    if (!list_section.empty()) {
      text = list_section;
      return;
    }

    if (list_offset != SIZE_MAX) {
      const std::string full = module->load_text(path);
      if (list_offset < full.size()) {
        text = full.substr(list_offset, std::min(list_size, full.size() - list_offset));
      }
      return;
    }
  }

  text = module->load_text(path);
}

void generator_source::drop_text() {
  text.clear();
  text.shrink_to_fit();
}

void generator_source::load_cold(const utils::safe_handle_t&) {
  ensure_text_loaded();
}

void generator_source::load_warm(const utils::safe_handle_t&) {}
void generator_source::unload_hot(const utils::safe_handle_t&) {}

void generator_source::unload_warm(const utils::safe_handle_t&) {
  drop_text();
}

const std::string& generator_config::source(const std::string_view& id) const {
  for (const auto& [known, text] : sources) {
    if (known == id) {
      return text;
    }
  }
  utils::error{}("originator generator '{}': no source text for '{}'", entry_id, id);
}

std::string read_generator_source(const demiurg::resource_system& resources, const std::string_view& id) {
  const auto absolute = demiurg::absolute_resource_path("", id);
  if (absolute.empty()) {
    utils::error{}("originator: '{}' is not a valid generator resource id", id);
  }
  return read_source(resources, absolute, "host");
}

void register_generator_resources(demiurg::resource_system& resources, const std::string& type_name) {
  resources.register_type<generator_source>(type_name, "tavl,lua,ds");
}

generator_config load_generator(const demiurg::resource_system& resources, const std::string_view& entry_id) {
  generator_config config;
  config.entry_id = demiurg::absolute_resource_path("", entry_id);
  if (config.entry_id.empty()) {
    utils::error{}("originator: '{}' is not a valid generator entry id", entry_id);
  }

  const auto entry_text = read_source(resources, config.entry_id, "generator entry");
  const auto entry = parse_entry(entry_text, config.entry_id);

  // Имя генератора берётся из документа, а если автор его не назвал — id точки входа. Пайплайн без
  // имени существовать может, но в сообщении об ошибке от него нет никакого толку.
  config.description.name = entry.name.empty() ? config.entry_id : entry.name;

  if (!entry.values.empty()) {
    const auto values_id = acquire(config, resources, entry.values, config.entry_id);
    config.description.values = parse_values(config.source(values_id), values_id);
    config.ranges = parse_value_ranges(config.source(values_id), values_id);
  }

  const auto buffers_id = acquire(config, resources, entry.buffers, config.entry_id);
  config.description.buffers = parse_buffers(config.source(buffers_id), buffers_id);
  for (const auto& shared : entry.shared_buffers) {
    const auto shared_id = acquire(config, resources, shared, config.entry_id);
    for (auto& buffer : parse_buffers(config.source(shared_id), shared_id)) {
      for (const auto& known : config.description.buffers) {
        if (known.name == buffer.name) {
          utils::error{}("originator generator '{}': buffer '{}' of shared document '{}' is declared twice",
                         config.entry_id, buffer.name, shared_id);
        }
      }
      config.description.buffers.push_back(std::move(buffer));
    }
  }

  // Входы пайплайна: имена буферов, приходящих извне. Переписывать в абсолютные id их не нужно —
  // это имена БУФЕРОВ, а не путей, и они уже объявлены в документе буферов.
  config.description.inputs = entry.inputs;

  config.description.steps = entry.steps;
  for (auto& step : config.description.steps) {
    // Тело и программы переписываются АБСОЛЮТНЫМИ id: дальше по ним же берётся текст, и второго
    // написания одного и того же пути в пакете не остаётся.
    step.body = acquire(config, resources, step.body, step.name);
    for (auto& [program_name, program_path] : step.programs) {
      program_path = acquire(config, resources, program_path, step.name);
    }
  }

  return config;
}

const generator_config& loaded_chain::config(const std::string_view& generator) const {
  for (const auto& [name, config] : generators) {
    if (name == generator) {
      return config;
    }
  }
  utils::error{}("originator chain '{}': generator '{}' is not a pipeline", description.name, generator);
  return generators.front().second;
}

loaded_chain load_chain(const demiurg::resource_system& resources, const std::string_view& chain_id) {
  const auto id = demiurg::absolute_resource_path("", chain_id);
  if (id.empty()) {
    utils::error{}("originator: '{}' is not a valid chain id", chain_id);
  }
  loaded_chain out;
  const auto text = read_source(resources, id, "generator chain");
  out.description = parse_chain(text, id);
  // Метаданные формы (роль, состав, фон, шумы и связи) меняют результат так же, как DS/Lua.
  out.setup.modules.push_back({true, "chain_document", id, text});
  if (!out.description.constants.empty()) {
    out.description.constants = demiurg::absolute_resource_path(id, out.description.constants);
    const auto source = read_source(resources, out.description.constants, "chain constants");
    out.setup.constants = parse_values(source, out.description.constants);
    out.setup.modules.push_back({true, "chain_constants", out.description.constants, source});
  }
  for (auto& module : out.description.modules) {
    module.path = demiurg::absolute_resource_path(id, module.path);
    out.setup.modules.push_back({false, module.name, module.path, read_source(resources, module.path, module.name)});
  }

  // Формулы видов форм входят в отпечаток каждого генератора, как модули: поле любого из них может их
  // читать.
  for (auto& shape : out.description.shapes) {
    shape.path = demiurg::absolute_resource_path(id, shape.path);
    shape.text = read_source(resources, shape.path, shape.name);
    out.setup.modules.push_back({true, shape.name, shape.path, shape.text});
    for (size_t i = 0; i < shape.parts.size(); ++i) {
      auto& part = shape.parts[i];
      part.path = demiurg::absolute_resource_path(id, part.path);
      part.text = read_source(resources, part.path, shape.name);
      out.setup.modules.push_back({true, shape.name + ".part" + std::to_string(i), part.path, part.text});
    }
  }

  for (auto& g : out.description.generators) {
    if (!g.entry.empty()) {
      g.entry = demiurg::absolute_resource_path(id, g.entry);
      auto config = load_generator(resources, g.entry);
      std::vector<script_text> scripts;
      std::string all;
      for (const auto& step : config.description.steps) {
        scripts.push_back({false, step.name, step.body, config.source(step.body)});
        for (const auto& [program_name, program_id] : step.programs) {
          scripts.push_back({true, program_name, program_id, config.source(program_id)});
        }
      }
      // Отпечаток — все тексты генератора в порядке их id: перестановка ссылок в точке входа не
      // меняет генератор, а правка любого файла меняет.
      auto sources = config.sources;
      std::sort(sources.begin(), sources.end());
      for (const auto& [source_id, source_text] : sources) {
        all += source_id;
        all += '\n';
        all += source_text;
        all += '\n';
      }
      out.setup.pipelines.emplace_back(g.name, config.description);
      out.setup.scripts.emplace_back(g.name, std::move(scripts));
      out.setup.fingerprints.emplace_back(g.name, std::move(all));
      out.generators.emplace_back(g.name, std::move(config));
    } else {
      if (g.buffers.empty()) {
        utils::error{}("originator chain '{}': native generator '{}' names no 'buffers' document", id, g.name);
      }
      g.buffers = demiurg::absolute_resource_path(id, g.buffers);
      const auto buffers_text = read_source(resources, g.buffers, g.name);
      out.setup.native_buffers.emplace_back(g.name, parse_buffers(buffers_text, g.buffers));
      out.setup.fingerprints.emplace_back(g.name, g.native + "\n" + buffers_text);
    }
  }
  return out;
}

} // namespace originator
} // namespace devils_engine
