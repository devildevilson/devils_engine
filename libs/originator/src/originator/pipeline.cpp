#include "devils_engine/originator/pipeline.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include <tavl/tavl.h>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

namespace devils_engine {
namespace originator {

namespace {
// Зеркала конфига. Имена полей = имена строк в tavl.
struct buffer_mirror {
  std::string name;
  std::vector<std::pair<std::string, std::string>> format;
  std::optional<std::string> layout;
  std::string size;
};

struct step_mirror {
  std::string name;
  std::string body;
  std::vector<std::string> reads;
  std::vector<std::string> writes;
  std::map<std::string, double> params;
  // Строковые параметры отдельной строкой конфига: tavl раскладывает документ по типам полей
  // структуры, поэтому одна карта не может держать и числа, и текст.
  std::map<std::string, std::string> strings;
  std::map<std::string, std::string> programs;
};

// Точка входа: та же строка шагов, но внутри одного документа. Отдельное зеркало, а не расширенное
// зеркало шага: поля точки входа и поля шага не должны перепутываться, иначе `values` внутри блока
// шага молча читалось бы как ссылка на документ.
struct entry_mirror {
  std::string name;
  std::string values;
  std::string buffers;
  // Буферы, которые пайплайн получает ИЗВНЕ: их заполняет хост до запуска, а не какой-то шаг. См.
  // pipeline_description::inputs.
  std::vector<std::string> inputs;
  std::vector<step_mirror> steps;
};

struct values_mirror {
  std::map<std::string, double> numbers;
  std::map<std::string, std::string> strings;
  // Диапазоны настройки: имя = [минимум, максимум, шаг]. Тройка, а не три отдельных документа,
  // потому что границы значения и его шаг — одно утверждение, и разъезжаться им незачем.
  std::map<std::string, std::vector<double>> ranges;
};

// Форма документа ДО разбора: список блоков (`{ ... } { ... }`) или одна структура (`имя = значение`).
//
// Проверка существует потому, что перепутанные формы НЕ дают ошибки разбора: строка верхнего уровня
// превращает весь документ в структуру, и все следующие блоки становятся её лишними значениями. Для
// зеркала шага это кончалось не диагностикой, а безостановочным чтением одного и того же места.
//
// Маркеры (строки, комментарии) пропускаются так же, как их пропускает deserialize_next, поэтому
// после классификации разбор начинается ровно с того же события.
bool document_is_list(tavl::parser& p) {
  for (;;) {
    const auto ev = p.peek();
    if (ev.type == tavl::event_type::eof || ev.type == tavl::event_type::not_enought_data) {
      return true; // пустой документ: список из нуля блоков
    }
    if (ev.type == tavl::event_type::row_begin || ev.type == tavl::event_type::row_end ||
        ev.type == tavl::event_type::empty_row || ev.type == tavl::event_type::got_comment) {
      p.poll_event();
      continue;
    }
    return ev.type == tavl::event_type::object_begin || ev.type == tavl::event_type::array_begin ||
           ev.type == tavl::event_type::tuple_begin;
  }
}

step_description make_step(step_mirror& mirror, const std::string_view& label, const size_t index) {
  if (mirror.name.empty()) {
    utils::error{}("originator: step {} in '{}' has no name", index, label);
  }
  if (mirror.body.empty()) {
    utils::error{}("originator step '{}': body must name a script resource", mirror.name);
  }

  step_description description;
  description.name = std::move(mirror.name);
  description.body = std::move(mirror.body);
  description.reads = std::move(mirror.reads);
  description.writes = std::move(mirror.writes);
  for (const auto& [key, value] : mirror.params) {
    description.params.set_number(key, value);
  }
  for (auto& [key, value] : mirror.strings) {
    description.params.set_string(key, std::move(value));
  }
  for (auto& [key, value] : mirror.programs) {
    description.programs.emplace_back(key, std::move(value));
  }
  return description;
}

void report_diagnostics(const tavl::ct_context& ctx, const std::string_view& label) {
  for (const auto& d : ctx.diagnostics) {
    if (!d.error.is_critical()) {
      continue;
    }
    utils::error{}("originator: could not parse '{}': error '{}' at {}:{} field '{}'",
                   label, tavl::to_string(d.error.type), d.error.span.line, d.error.span.column, d.field);
  }
}
} // namespace

std::vector<buffer_description> parse_buffers(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  // Поле буфера пишется ОПЕРАТОРОМ (`position = v3`), а не кортежем (`(position, v3)`): лишние
  // скобки в документе-схеме ничего не сообщают, а строка с оператором читается как строка таблицы.
  //
  // Разбор от этого не меняется, и это ключевое: строка с оператором — та же ПАРА, что и строка в
  // скобках, потому что чтение пары пропускает операторные лексемы между слотами. Поэтому оба
  // написания читаются одинаково, и старые документы остаются валидными.
  //
  // Просилось здесь ДВОЕТОЧИЕ (`position : v3`), но `:` не входит в `operator_chars` самого tavl,
  // поэтому зарегистрировать его нельзя — проверка при регистрации падает. Правка в tavl готова
  // (двоеточие добавлено в набор, датавремя от этого не страдает, потому что оно распознаётся до
  // операторов), и как только она выйдет релизом, формат можно перевести на `:` одной строкой:
  // достаточно зарегистрировать оператор здесь, а сами документы примут любое из двух написаний.
  p.flush(std::string(text));
  p.finish();

  tavl::ct_context ctx;
  std::vector<buffer_description> result;

  buffer_mirror mirror{};
  while (tavl::deserialize_next(p, ctx, mirror)) {
    if (mirror.name.empty()) {
      utils::error{}("originator: buffer {} in '{}' has no name", result.size(), label);
    }
    if (mirror.size.empty()) {
      utils::error{}("originator buffer '{}': size must name a declared constant", mirror.name);
    }

    const auto storage = mirror.layout.has_value() ? parse_storage_kind(*mirror.layout) : storage_kind::aos;
    if (storage >= storage_kind::count) {
      utils::error{}("originator buffer '{}': unknown layout '{}', expected aos or soa", mirror.name, *mirror.layout);
    }

    std::vector<std::pair<std::string_view, std::string_view>> fields;
    fields.reserve(mirror.format.size());
    for (const auto& [field_name, spelling] : mirror.format) {
      fields.emplace_back(field_name, spelling);
    }

    buffer_description description;
    description.name = mirror.name;
    description.layout = make_buffer_layout(storage, fields, mirror.name);
    description.size_name = mirror.size;
    result.push_back(std::move(description));

    mirror = buffer_mirror{};
  }

  report_diagnostics(ctx, label);
  return result;
}

std::vector<step_description> parse_steps(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(text));
  p.finish();

  if (!document_is_list(p)) {
    utils::error{}("originator: '{}' is a list of step blocks, but starts with a row of its own; "
                   "a generator that names its own values and buffers is an ENTRY document (parse_entry), "
                   "where the steps live inside 'steps = [ ... ]'",
                   label);
  }

  tavl::ct_context ctx;
  std::vector<step_description> result;

  step_mirror mirror{};
  while (tavl::deserialize_next(p, ctx, mirror)) {
    result.push_back(make_step(mirror, label, result.size()));
    mirror = step_mirror{};
  }

  report_diagnostics(ctx, label);
  return result;
}

pipeline_entry parse_entry(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(text));
  p.finish();

  if (document_is_list(p)) {
    utils::error{}("originator: '{}' is a bare list of blocks, not an entry document; "
                   "an entry names its parts ('values', 'buffers') and carries its steps in 'steps = [ ... ]'",
                   label);
  }

  tavl::ct_context ctx;
  entry_mirror mirror{};
  tavl::deserialize_next(p, ctx, mirror);
  report_diagnostics(ctx, label);

  if (mirror.buffers.empty()) {
    utils::error{}("originator entry '{}': 'buffers' must name the document of buffer declarations", label);
  }
  if (mirror.steps.empty()) {
    utils::error{}("originator entry '{}': 'steps' is empty, so the generator does nothing", label);
  }

  pipeline_entry entry;
  entry.name = std::move(mirror.name);
  entry.values = std::move(mirror.values);
  entry.buffers = std::move(mirror.buffers);
  entry.inputs = std::move(mirror.inputs);
  entry.steps.reserve(mirror.steps.size());
  for (size_t i = 0; i < mirror.steps.size(); ++i) {
    entry.steps.push_back(make_step(mirror.steps[i], label, i));
  }
  return entry;
}

parameters parse_values(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(text));
  p.finish();

  tavl::ct_context ctx;
  parameters result;

  values_mirror mirror{};
  while (tavl::deserialize_next(p, ctx, mirror)) {
    for (const auto& [key, value] : mirror.numbers) {
      result.set_number(key, value);
    }
    for (auto& [key, value] : mirror.strings) {
      result.set_string(key, std::move(value));
    }
    mirror = values_mirror{};
  }

  report_diagnostics(ctx, label);
  return result;
}

double value_range::clamp(const double value) const noexcept {
  return std::min(std::max(value, minimum), maximum);
}

double value_range::advance(const double value, const int64_t steps) const noexcept {
  if (step <= 0.0) {
    return clamp(value);
  }
  // Позиция считается в шагах ОТ МИНИМУМА и округляется: так значение всегда лежит на сетке шага,
  // и повторные «плюс-минус» не оставляют дробного хвоста.
  const double position = std::round((value - minimum) / step) + double(steps);
  return clamp(minimum + position * step);
}

std::vector<value_range> parse_value_ranges(const std::string_view& text, const std::string_view& label) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(text));
  p.finish();

  tavl::ct_context ctx;
  std::vector<value_range> result;

  values_mirror mirror{};
  while (tavl::deserialize_next(p, ctx, mirror)) {
    for (const auto& [key, triple] : mirror.ranges) {
      if (triple.size() != 3) {
        utils::error{}("originator: range '{}' in '{}' needs exactly [minimum, maximum, step], got {} values",
                       key, label, triple.size());
      }
      if (triple[1] <= triple[0] || triple[2] <= 0.0) {
        utils::error{}("originator: range '{}' in '{}' is empty or has a non-positive step: [{}, {}, {}]",
                       key, label, triple[0], triple[1], triple[2]);
      }
      result.push_back(value_range{key, triple[0], triple[1], triple[2]});
    }
    mirror = values_mirror{};
  }

  report_diagnostics(ctx, label);
  return result;
}

void size_table::set(const std::string_view& name, const size_t value) {
  for (auto& entry : entries_) {
    if (entry.first == name) {
      entry.second = value;
      return;
    }
  }
  entries_.emplace_back(std::string(name), value);
}

bool size_table::has(const std::string_view& name) const noexcept {
  return std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.first == name; });
}

size_t size_table::get(const std::string_view& name, const std::string_view& buffer_name) const {
  for (const auto& entry : entries_) {
    if (entry.first == name) {
      return entry.second;
    }
  }
  utils::error{}("originator buffer '{}': size '{}' is not declared by the host", buffer_name, name);
}

buffer* step_context::find_write(const std::string_view& buffer_name) const noexcept {
  for (auto* candidate : writes) {
    if (candidate != nullptr && candidate->name() == buffer_name) {
      return candidate;
    }
  }
  return nullptr;
}

const buffer* step_context::find_read(const std::string_view& buffer_name) const noexcept {
  for (const auto* candidate : reads) {
    if (candidate != nullptr && candidate->name() == buffer_name) {
      return candidate;
    }
  }
  // Буфер, привязанный на запись, доступен и на чтение — своему шагу он уже принадлежит.
  return find_write(buffer_name);
}

pipeline::pipeline(pipeline_description description, const size_table& sizes, const uint64_t seed) :
  description_(std::move(description)), seed_(seed) {
  build(sizes);
  validate();
}

void pipeline::build(const size_table& sizes) {
  buffers_.reserve(description_.buffers.size());
  for (const auto& declaration : description_.buffers) {
    const size_t count = sizes.get(declaration.size_name, declaration.name);
    buffers_.push_back(std::make_unique<buffer>(declaration.name, declaration.layout, count));
  }

  step_writes_.resize(description_.steps.size());
  step_reads_.resize(description_.steps.size());
  step_params_.resize(description_.steps.size());

  for (size_t i = 0; i < description_.steps.size(); ++i) {
    const auto& step = description_.steps[i];

    step_params_[i] = description_.values;
    step_params_[i].overlay(step.params);

    for (const auto& name : step.writes) {
      auto* target = find_buffer(name);
      if (target == nullptr) {
        utils::error{}("originator step '{}': writes '{}', which is not a declared buffer", step.name, name);
      }
      step_writes_[i].push_back(target);
    }

    for (const auto& name : step.reads) {
      auto* source = find_buffer(name);
      if (source == nullptr) {
        utils::error{}("originator step '{}': reads '{}', which is not a declared buffer", step.name, name);
      }
      step_reads_[i].push_back(source);
    }
  }
}

void pipeline::validate() const {
  for (size_t i = 0; i < description_.buffers.size(); ++i) {
    for (size_t j = i + 1; j < description_.buffers.size(); ++j) {
      if (description_.buffers[i].name == description_.buffers[j].name) {
        utils::error{}("originator pipeline '{}': duplicate buffer '{}'", description_.name, description_.buffers[i].name);
      }
    }
  }

  // Буфер считается валидным начиная с шага, который его пишет. Чтение раньше первой записи — это
  // чтение неопределённых данных, и молчать про это нельзя.
  //
  // ВХОДЫ ПАЙПЛАЙНА считаются записанными с самого начала. Понятие входа появилось из двухмасштабной
  // генерации: грубый мировой проход считается ОДИН раз и отдаёт каркас (маршруты, узлы, сводки), а
  // чанковый пайплайн читает из него то, что попадает в свою область. Заполняет такой буфер ХОСТ —
  // он один знает, какая часть каркаса резидентна и что попадает в область этого чанка, — поэтому ни
  // одного шага, который его пишет, не существует и существовать не должно.
  //
  // Объявляется это в точке входа (`inputs = [ ... ]`), а не выводится: «буфер, который читают, но
  // никто не пишет» — это ровно то, о чём библиотека обязана кричать, и отличить приход извне от
  // опечатки в имени можно только по объявлению автора.
  std::vector<const buffer*> written;
  for (const auto& name : description_.inputs) {
    const auto found = std::find_if(buffers_.begin(), buffers_.end(),
                                    [&name](const auto& candidate) { return candidate->name() == name; });
    if (found == buffers_.end()) {
      utils::error{}("originator pipeline '{}': input '{}' is not among its declared buffers", description_.name,
                     name);
    }
    written.push_back(found->get());
  }

  for (size_t i = 0; i < description_.steps.size(); ++i) {
    const auto& step = description_.steps[i];

    for (size_t j = i + 1; j < description_.steps.size(); ++j) {
      if (step.name == description_.steps[j].name) {
        utils::error{}("originator pipeline '{}': duplicate step '{}'", description_.name, step.name);
      }
    }

    // Повтор в writes — почти наверняка опечатка конфига: буфер попадёт в published_after дважды.
    for (size_t a = 0; a < step.writes.size(); ++a) {
      for (size_t b = a + 1; b < step.writes.size(); ++b) {
        if (step.writes[a] == step.writes[b]) {
          utils::error{}("originator step '{}': buffer '{}' is listed twice in writes", step.name, step.writes[a]);
        }
      }
    }
    for (size_t a = 0; a < step.reads.size(); ++a) {
      for (size_t b = a + 1; b < step.reads.size(); ++b) {
        if (step.reads[a] == step.reads[b]) {
          utils::error{}("originator step '{}': buffer '{}' is listed twice in reads", step.name, step.reads[a]);
        }
      }
    }

    for (const auto& read_name : step.reads) {
      const auto same = std::find(step.writes.begin(), step.writes.end(), read_name);
      if (same != step.writes.end()) {
        utils::error{}("originator step '{}': buffer '{}' is bound for writing, remove it from reads — "
                       "a written buffer is readable by its own step",
                       step.name, read_name);
      }
    }

    for (const auto* source : step_reads_[i]) {
      if (std::find(written.begin(), written.end(), source) == written.end()) {
        utils::error{}("originator step '{}': reads '{}' before any step has written it", step.name, source->name());
      }
    }

    for (const auto* target : step_writes_[i]) {
      if (std::find(written.begin(), written.end(), target) == written.end()) {
        written.push_back(target);
      }
    }
  }

  // Буфер, который ни один шаг не читает и не пишет, — объявленная и оплаченная память, которой
  // никто не пользуется. Это не ошибка (host может заполнять его сам), но почти всегда остаток от
  // правки конфига, поэтому о нём стоит сказать.
  for (const auto& candidate : buffers_) {
    bool bound = false;
    for (size_t i = 0; i < description_.steps.size() && !bound; ++i) {
      for (const auto* target : step_writes_[i]) {
        bound = bound || target == candidate.get();
      }
      for (const auto* source : step_reads_[i]) {
        bound = bound || source == candidate.get();
      }
    }
    // Вход пайплайна, который никто не читает, — тоже остаток от правки конфига, но говорить про него
    // надо иначе: он не «не привязан», он привезён и не понадобился.
    const bool declared_input =
      std::find(description_.inputs.begin(), description_.inputs.end(), candidate->name()) !=
      description_.inputs.end();
    if (!bound && declared_input) {
      utils::warn("originator pipeline '{}': input '{}' is filled from outside ({} bytes) but no step reads it",
                  description_.name, candidate->name(), candidate->byte_size());
    } else if (!bound) {
      utils::warn("originator pipeline '{}': buffer '{}' is declared and allocated ({} bytes) but no step binds it",
                  description_.name, candidate->name(), candidate->byte_size());
    }
  }
}

const std::string& pipeline::name() const noexcept {
  return description_.name;
}

uint64_t pipeline::seed() const noexcept {
  return seed_;
}

const chunk_key& pipeline::chunk() const noexcept {
  return chunk_;
}

bool pipeline::chunked() const noexcept {
  return chunked_;
}

void pipeline::set_chunk(const chunk_key& key) noexcept {
  chunk_ = key;
  chunked_ = true;
}

void pipeline::clear_buffers() noexcept {
  for (auto& candidate : buffers_) {
    candidate->clear();
  }
}

size_t pipeline::buffer_count() const noexcept {
  return buffers_.size();
}

buffer& pipeline::buffer_at(const size_t index) noexcept {
  return *buffers_[index];
}

buffer* pipeline::find_buffer(const std::string_view& buffer_name) noexcept {
  for (auto& candidate : buffers_) {
    if (candidate->name() == buffer_name) {
      return candidate.get();
    }
  }
  return nullptr;
}

size_t pipeline::total_byte_size() const noexcept {
  size_t total = 0;
  for (const auto& candidate : buffers_) {
    total += candidate->byte_size();
  }
  return total;
}

size_t pipeline::step_count() const noexcept {
  return description_.steps.size();
}

const step_description& pipeline::step_at(const size_t index) const noexcept {
  return description_.steps[index];
}

std::span<buffer* const> pipeline::published_after(const size_t index) const noexcept {
  if (index >= step_writes_.size()) {
    return {};
  }
  return step_writes_[index];
}

void pipeline::run_step(const size_t index, const step_invoker& invoker) {
  if (index >= description_.steps.size()) {
    utils::error{}("originator pipeline '{}': step {} is out of range", description_.name, index);
  }

  const auto& step = description_.steps[index];

  step_context context;
  context.name = step.name;
  context.index = index;
  // Зерно шага — функция от зерна пайплайна и ИМЕНИ шага, а не позиция в потоке случайности:
  // перестановка или перезапуск шага не сдвигает случайность остальных.
  const auto step_hash = std::hash<std::string>{}(step.name);
  context.seed = utils::mix(seed_, step_hash);
  // Второе зерно домешивает ключ чанка. Оба чистые функции, поэтому чанк не зависит от того, какие
  // чанки посчитаны раньше — ни через одно, ни через другое.
  context.chunk_seed = utils::mix(seed_, step_hash, uint64_t(chunk_.x), uint64_t(chunk_.y), uint64_t(chunk_.z));
  context.chunk = chunk_;
  context.chunked = chunked_;
  context.params = &step_params_[index];
  context.programs = step.programs;
  context.writes = step_writes_[index];
  context.reads = step_reads_[index];

  invoker(context);
}

void pipeline::run(const step_invoker& invoker) {
  for (size_t i = 0; i < description_.steps.size(); ++i) {
    run_step(i, invoker);
  }
}

} // namespace originator
} // namespace devils_engine
