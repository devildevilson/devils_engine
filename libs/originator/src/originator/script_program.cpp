#include "script_math.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/originator/script_program.h"
#include "devils_engine/originator/script_column.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>
#include <devils_script/type_traits.h>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/prng.h"

// Исполнение `devils_script` над плотным буфером: компиляция против имён привязок и проход по
// диапазону.
//
// DS root хранит указатель на локальный execution frame (в пределах 16 байт VM).
// Pointwise читает свой индекс; gather явно разделяет own/other bindings и выбирает
// другой индекс только у other полей. Адреса записи программе не доступны.
// Saved slots и lists сбрасываются перед каждым элементом/кандидатом. Сам context
// живёт на chunk; перенос состояния между элементами сделал бы результат зависимым
// от partitioning. Column subset чистый; VM-only конструкции сохраняют свой путь.
//
// ТИПЫ ПРОВЕРЯЮТСЯ НА РАЗБОРЕ, а не на исполнении (система собирается с небезопасными опкодами):
// платить за проверку типа стека на каждом из миллионов элементов незачем, а программа, не сошедшаяся
// по типам, до исполнения не доходит вовсе.

namespace devils_engine {
namespace originator {

namespace {
namespace ds = devils_script;

// Скоуп скрипта — ОДИН элемент. Влезает в 16 байт стека devils_script, тривиально разрушается.
// Больше в нём ничего нет намеренно: из этой пары нельзя добраться ни до соседа, ни до записи.
struct script_execution {
  const const_field_accessor* accessors = nullptr;
  uint64_t index = 0;
  uint32_t component = 0;
  size_t own_inputs = SIZE_MAX;
  size_t other_index = 0;
  const double* parameters = nullptr;
};
struct script_element {
  const script_execution* execution = nullptr;
  bool valid() const noexcept { return execution && execution->accessors; }
};

double read_result_component(const script_element element) { return element.execution->component; }
double read_element_index(const script_element element) { return double(element.execution->index); }
double read_other_index(const script_element element) { return double(element.execution->other_index); }

static_assert(sizeof(script_element) <= 16, "devils_script scope must fit the VM stack element");
static_assert(std::is_trivially_destructible_v<script_element>);

// Аксессор k-го входного поля. Индекс элемента ВСЕГДА берётся из скоупа, поэтому обращение к
// чужому элементу не выражается на уровне регистрации.
template<size_t index>
double read_parameter(const script_element element) { return element.execution->parameters[index]; }
template<size_t... indices>
void register_parameters(ds::system& system, std::span<const std::string> names, std::index_sequence<indices...>) {
  ((indices < names.size() ? void(system.register_function<&read_parameter<indices>>("param_" + names[indices])) : void()), ...);
}

template <size_t index, uint32_t component = 0>
double read_input(const script_element element) {
  return element.execution->accessors[index].get(index < element.execution->own_inputs ? element.execution->index : element.execution->other_index, component);
}

template <size_t... indices>
void register_readers(ds::system& system, const std::span<const std::string>& names, std::index_sequence<indices...>) {
  ((indices < names.size() ? void(system.register_function<&read_input<indices>>(names[indices])) : void()), ...);
}
template<size_t index>
void register_components(ds::system& system, const std::span<const std::string> names,
                         const std::span<const field_type> types) {
  if (index >= names.size() || types.empty() || types[index].components <= 1) return;
  const auto& name = names[index];
  system.register_function<&read_input<index, 0>>(name + "_x");
  if (types[index].components > 1) system.register_function<&read_input<index, 1>>(name + "_y");
  if (types[index].components > 2) system.register_function<&read_input<index, 2>>(name + "_z");
  if (types[index].components > 3) system.register_function<&read_input<index, 3>>(name + "_w");
}

template<size_t... indices>
void register_component_readers(ds::system& system, const std::span<const std::string> names,
                                const std::span<const field_type> types, std::index_sequence<indices...>) {
  (register_components<indices>(system, names, types), ...);
}
} // namespace

// Один объявленный `ctx:arg:` скрипта: имя, слот в контексте и то, каким типом его выставлять.
struct script_argument {
  enum class kind { number, integer, boolean };

  std::string name;
  size_t slot = 0;
  kind value_kind = kind::number;
};

struct script_program::implementation {
  std::string name;
  std::vector<std::string> input_names;
  std::vector<field_type> input_types;
  std::vector<std::string> parameter_names;
  std::unique_ptr<column_program> column;
  std::vector<std::pair<size_t, uint32_t>> column_bindings;
  std::vector<std::string> argument_names;
  std::vector<script_argument> arguments;
  result_kind kind = result_kind::number;
  ds::system system;
  ds::container program;
};

script_program::script_program() noexcept = default;
script_program::~script_program() noexcept = default;

std::unique_ptr<script_program> script_program::compile(const std::string_view& name,
                                                        const std::string_view& source,
                                                        const std::span<const std::string>& input_names,
                                                        const result_kind kind, const std::span<const field_type> input_types,
                                                        const std::span<const std::string> parameter_names, const script_backend backend) {
  if (parameter_names.size() > max_script_parameters) utils::error{}("originator script '{}': too many parameter bindings", name);
  if (!input_types.empty() && input_types.size() != input_names.size())
    utils::error{}("originator script '{}': input types do not match names", name);
  std::vector<std::string> reader_names(input_names.begin(), input_names.end());
  static constexpr const char* suffix[]{"_x", "_y", "_z", "_w"};
  for (size_t i = 0; i < input_types.size(); ++i) {
    if (!input_types[i].valid()) utils::error{}("originator script '{}': invalid input type at {}", name, i);
    if (input_types[i].components <= 1) continue;
    for (uint32_t c = 0; c < input_types[i].components; ++c) {
      const auto alias = input_names[i] + suffix[c];
      if (std::find(reader_names.begin(), reader_names.end(), alias) != reader_names.end())
        utils::error{}("originator script '{}': ambiguous component name '{}'", name, alias);
      reader_names.push_back(alias);
    }
  }
  if (input_names.size() > max_script_inputs) {
    utils::error{}("originator script '{}': {} input fields, at most {} are supported",
                   name, input_names.size(), max_script_inputs);
  }

  for (const auto& field : reader_names) {
    if (field == "result_component" || field == "element_index" || field == "other_index")
      utils::error{}("originator script '{}': '{}' is reserved execution data", name, field);
  }
  std::vector<std::string> registered = reader_names;
  for (const auto& param : parameter_names) {
    const auto binding = "param_" + param;
    if (param.empty() || std::find(registered.begin(), registered.end(), binding) != registered.end())
      utils::error{}("originator script '{}': ambiguous parameter binding '{}'", name, binding);
    registered.push_back(binding);
  }
  for (size_t i = 0; i < input_names.size(); ++i) {
    if (input_names[i].empty()) {
      utils::error{}("originator script '{}': input {} has an empty name", name, i);
    }
    for (size_t j = i + 1; j < input_names.size(); ++j) {
      if (input_names[i] == input_names[j]) {
        utils::error{}("originator script '{}': two inputs share the name '{}' — the script could not tell them apart",
                       name, input_names[i]);
      }
    }
  }

  auto program = std::unique_ptr<script_program>(new script_program());
  program->impl_ = std::make_unique<implementation>();
  auto& impl = *program->impl_;

  impl.name.assign(name);
  impl.input_names.assign(input_names.begin(), input_names.end());
  impl.input_types.assign(input_types.begin(), input_types.end());
  impl.parameter_names.assign(parameter_names.begin(), parameter_names.end());
  impl.argument_names.assign(parameter_names.begin(), parameter_names.end());
  impl.kind = kind;

  try {
    impl.system.init_basic_functions();
    impl.system.init_math();
    impl.system.register_function<&read_result_component>("result_component");
    impl.system.register_function<&read_element_index>("element_index");
    impl.system.register_function<&read_other_index>("other_index");
    detail::add_generation_math(impl.system);
    register_parameters(impl.system, parameter_names, std::make_index_sequence<max_script_parameters>{});
    register_readers(impl.system, input_names, std::make_index_sequence<max_script_inputs>{});
    register_component_readers(impl.system, input_names, input_types, std::make_index_sequence<max_script_inputs>{});

    // Небезопасные варианты опкодов: проверка типов остаётся на разборе (см. шапку файла).
    impl.system.toggle_safety();

    impl.program = kind == result_kind::predicate
                     ? impl.system.parse<bool, script_element>(name, source)
                     : impl.system.parse<double, script_element>(name, source);
  } catch (const utils::critical_error&) {
    throw;
  } catch (const std::exception& error) {
    utils::error{}("originator script '{}': could not compile: {}", name, error.what());
  }

  // Слот 0 занимает корневой скоуп (сам элемент), именованные аргументы идут с первого. Их типы
  // проверяются здесь, а не при первом исполнении: непонятный тип аргумента — ошибка конфига.
  for (size_t slot = 1; slot < impl.program.args.size(); ++slot) {
    const auto argument_name = impl.program.get_arg_name(slot);
    if (argument_name.empty()) {
      continue;
    }

    const auto type = impl.program.args[slot].type;
    script_argument argument;
    argument.name.assign(argument_name);
    argument.slot = slot;

    if (type == ds::utils::type_name<double>()) {
      argument.value_kind = script_argument::kind::number;
    } else if (type == ds::utils::type_name<int64_t>()) {
      argument.value_kind = script_argument::kind::integer;
    } else if (type == ds::utils::type_name<bool>()) {
      argument.value_kind = script_argument::kind::boolean;
    } else {
      utils::error{}("originator script '{}': argument '{}' has type '{}'; only number, integer and "
                     "boolean arguments can come from step parameters",
                     name, argument_name, type);
    }

    impl.argument_names.push_back(argument.name);
    impl.arguments.push_back(std::move(argument));
  }

  std::vector<std::string> columns;
  for (size_t i = 0; i < input_names.size(); ++i) {
    columns.push_back(input_names[i]);
    impl.column_bindings.emplace_back(i, 0);
    if (input_types.empty() || input_types[i].components <= 1) continue;
    for (uint32_t c = 0; c < input_types[i].components; ++c) {
      columns.push_back(input_names[i] + suffix[c]);
      impl.column_bindings.emplace_back(i, c);
    }
  }
  columns.push_back("element_index"); impl.column_bindings.emplace_back(SIZE_MAX, 0);
  columns.push_back("other_index"); impl.column_bindings.emplace_back(SIZE_MAX, 1);
  std::vector<std::string> scalars;
  for (const auto& parameter : parameter_names) scalars.push_back("param_" + parameter);
  for (const auto& argument : impl.arguments) scalars.push_back("ctx:arg:" + argument.name);
  scalars.push_back("result_component");
  if (backend == script_backend::automatic) {
    auto column = column_program::try_compile(name, source, columns, scalars, kind == result_kind::predicate);
    if (column) impl.column = std::move(*column);
  }
  return program;
}

const std::vector<std::string>& script_program::argument_names() const noexcept {
  return impl_->argument_names;
}

const std::string& script_program::name() const noexcept {
  return impl_->name;
}

const std::vector<field_type>& script_program::input_types() const noexcept { return impl_->input_types; }

size_t script_program::input_count() const noexcept {
  return impl_->input_names.size();
}

script_program::result_kind script_program::kind() const noexcept {
  return impl_->kind;
}

aperture::values script_program::shape() const noexcept {
  // Не выбор автора, а следствие того, что зарегистрировано: чтение своего элемента и возврат
  // значения. Ни записи, ни соседей в системе нет.
  return aperture::pointwise;
}

const std::vector<std::string>& script_program::input_names() const noexcept {
  return impl_->input_names;
}

struct program_frame {
  const script_program::implementation& impl;
  ds::context vm;
  script_execution execution;
  script_element element;
  size_t saved_count;
  std::array<double, max_script_parameters> uniform_values{};
  std::vector<double> column_values;
  std::vector<const double*> column_pointers;
  std::vector<double> scalar_values;
  column_workspace column_scratch;
  program_frame(const script_program::implementation& state, std::span<const const_field_accessor> accessors,
                const parameters& params, const size_t own_inputs = SIZE_MAX) :
    impl(state), vm(std::max<size_t>(impl.program.max_stack, 1), std::max<size_t>(impl.program.max_saved, 1)),
    execution{accessors.data(), 0, 0, own_inputs, 0}, element{&execution},
    saved_count(std::min(impl.program.max_saved, vm.saved_stack._size)) {
    vm.create_lists(&impl.program);
    for (size_t i = 0; i < impl.parameter_names.size(); ++i) uniform_values[i] = params.number(impl.parameter_names[i]);
    execution.parameters = uniform_values.data();
    for (const auto& argument : impl.arguments) {
      switch (argument.value_kind) {
        case script_argument::kind::number: vm.set_arg(argument.slot, params.number(argument.name)); break;
        case script_argument::kind::integer: vm.set_arg(argument.slot, params.integer(argument.name)); break;
        case script_argument::kind::boolean: vm.set_arg(argument.slot, params.number(argument.name) != 0.0); break;
      }
    }
    if (impl.column) {
      column_values.resize(impl.column_bindings.size());
      column_pointers.resize(column_values.size());
      for (size_t i = 0; i < column_values.size(); ++i) column_pointers[i] = &column_values[i];
      for (const auto& name : impl.parameter_names) scalar_values.push_back(params.number(name));
      for (const auto& arg : impl.arguments) scalar_values.push_back(arg.value_kind == script_argument::kind::integer ?
        double(params.integer(arg.name)) : arg.value_kind == script_argument::kind::boolean ? double(params.number(arg.name) != 0) : params.number(arg.name));
      scalar_values.push_back(0);
    }
  }
  double evaluate(const size_t node, const size_t other, const uint32_t component, const uint64_t seed) {
    if (impl.column) {
      for (size_t i = 0; i < impl.column_bindings.size(); ++i) {
        if (!impl.column->used_columns()[i]) continue;
        const auto [field, part] = impl.column_bindings[i];
        column_values[i] = field == SIZE_MAX ? double(part == 0 ? node : other) :
          execution.accessors[field].get(field < execution.own_inputs ? node : other, part);
      }
      scalar_values.back() = component;
      double value = 0;
      impl.column->run(1, column_pointers, scalar_values, &value, column_scratch);
      return value;
    }
    execution.index = node;
    execution.other_index = other;
    execution.component = component;
    vm.clear();
    // Lists принадлежат одному элементу/кандидату, как saved slots: context::clear
    // сбрасывает VM frames, но намеренно не очищает внешнее хранилище списков.
    for (auto& list : vm.lists) list.clear();
    for (size_t slot = 0; slot < saved_count; ++slot) {
      vm.saved_stack._data[slot] = ds::stack_element{};
      vm.saved_stack._types[slot] = std::string_view{};
    }
    vm.prng_state = seed;
    vm.set_arg(0, element);
    impl.program.process(&vm);
    return impl.kind == script_program::result_kind::predicate ? (vm.get_return<bool>() ? 1.0 : 0.0) : vm.get_return<double>();
  }
};

void run_chunk(const script_program::implementation& impl, const std::span<const const_field_accessor>& accessors,
               const field_accessor& output, const bool, const parameters& params, const uint64_t seed,
               const size_t begin, const size_t end, const uint32_t output_component, const uint32_t result_component) {
  program_frame frame(impl, accessors, params);
  if (impl.column) {
    std::vector<std::array<double, column_program::lanes>> data;
    std::vector<size_t> used;
    std::vector<const double*> pointers(impl.column_bindings.size(), nullptr);
    for (size_t i = 0; i < impl.column_bindings.size(); ++i)
      if (impl.column->used_columns()[i]) used.push_back(i);
    data.resize(used.size());
    for (size_t i = 0; i < used.size(); ++i) pointers[used[i]] = data[i].data();
    frame.scalar_values.back() = result_component;
    std::array<double, column_program::lanes> result;
    for (size_t first = begin; first < end; first += column_program::lanes) {
      const auto n = std::min(column_program::lanes, end - first);
      for (size_t i = 0; i < used.size(); ++i) {
        const auto [field, component] = impl.column_bindings[used[i]];
        for (size_t lane = 0; lane < n; ++lane)
          data[i][lane] = field == SIZE_MAX ? double(first + lane) : accessors[field].get(first + lane, component);
      }
      impl.column->run(n, pointers, frame.scalar_values, result.data(), frame.column_scratch);
      for (size_t lane = 0; lane < n; ++lane) output.set(first + lane, result[lane], output_component);
    }
    return;
  }
  for (size_t i = begin; i < end; ++i)
    output.set(i, frame.evaluate(i, i, result_component, utils::mix(seed, i)), output_component);
}

std::optional<size_t> script_footprint(const script_program& program, const size_t workers) {
  const auto& impl = *program.impl_;
  if (impl.program.max_lists || !impl.program.lists.empty()) return std::nullopt;
  // Контекст на chunk: у КАЖДОГО VM stack есть и values, и type tags. Временные
  // vector push_back учитываются с запасом на capacity; allocator bookkeeping не считается.
  const size_t slot = sizeof(ds::stack_element) + sizeof(std::string_view);
  const size_t stack = std::max<size_t>(impl.program.max_stack, 1) * slot;
  const size_t saved = std::max<size_t>(impl.program.max_saved, 1) * slot;
  const size_t args = ds::context::script_arguments_size * slot;
  const size_t column = impl.column ? impl.column->workspace_bytes() +
    impl.column_bindings.size() * (sizeof(double) + 2 * sizeof(const double*) + 2 * sizeof(size_t) + sizeof(std::array<double, column_program::lanes>)) +
    2 * (impl.parameter_names.size() + impl.arguments.size() + 1) * sizeof(double) : 0;
  const size_t fixed = sizeof(program_frame) + sizeof(std::array<double, column_program::lanes>) +
    2 * impl.input_names.size() * sizeof(const_field_accessor);
  const size_t per_worker = stack + saved + args + column + fixed;
  const auto count = std::max<size_t>(workers, 1);
  if (per_worker > SIZE_MAX / count) return std::nullopt;
  return per_worker * count;
}

dispatch_check check_script_dispatch(const script_program& program,
                                    const std::span<const field_ref>& inputs,
                                    const std::span<const field_ref>& outputs,
                                    const parameters& params,
                                    const size_t range_begin,
                                    const size_t range_end,
                                    const std::string_view& step_name, const uint32_t output_component) {
  dispatch_check result;
  result.parallel = is_parallel(program.shape());

  const auto& name = program.name();
  const auto& input_names = program.input_names();

  const auto fail = [&](std::string message) {
    result.allowed = false;
    result.parallel = false;
    result.message = std::move(message);
    return result;
  };

  if (inputs.size() != input_names.size()) {
    return fail(std::format("step '{}': script '{}' was compiled against {} inputs, got {}",
                            step_name, name, input_names.size(), inputs.size()));
  }
  if (outputs.size() != 1) {
    return fail(std::format("step '{}': script '{}' writes exactly one field, got {}",
                            step_name, name, outputs.size()));
  }

  // Имя привязки обязано совпасть с тем, под которым поле видно в скрипте: иначе скрипт считал бы
  // одно, а получал другое, и это не заметно ни в конфиге, ни в тексте скрипта.
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (!inputs[i].valid()) {
      return fail(std::format("step '{}': script '{}' got an invalid input {}", step_name, name, i));
    }
    if (!program.input_types().empty() && inputs[i].type().components != program.input_types()[i].components)
      return fail(std::format("step '{}': script '{}' input '{}' changed component count", step_name, name, input_names[i]));
    if (inputs[i].field_name() != input_names[i]) {
      return fail(std::format("step '{}': script '{}' reads input {} as '{}', but the binding is '{}.{}'",
                              step_name, name, i, input_names[i], inputs[i].buffer_name(), inputs[i].field_name()));
    }
  }

  const auto& target = outputs.front();
  if (!target.valid()) {
    return fail(std::format("step '{}': script '{}' got an invalid output", step_name, name));
  }
  if (!target.writable()) {
    return fail(std::format("step '{}': script '{}' writes to '{}.{}', but that buffer is bound for reading",
                            step_name, name, target.buffer_name(), target.field_name()));
  }
  if (output_component >= target.type().components)
    return fail(std::format("step '{}': script '{}' output component {} is outside '{}'", step_name, name, output_component, target.field_name()));
  if (range_end < range_begin) {
    return fail(std::format("step '{}': script '{}' got an inverted range [{}, {})",
                            step_name, name, range_begin, range_end));
  }
  if (range_end > target.count()) {
    return fail(std::format("step '{}': script '{}' range [{}, {}) exceeds '{}' with {} elements",
                            step_name, name, range_begin, range_end, target.buffer_name(), target.count()));
  }
  for (const auto& binding : inputs) {
    if (range_end > binding.count()) {
      return fail(std::format("step '{}': script '{}' range [{}, {}) exceeds '{}' with {} elements",
                              step_name, name, range_begin, range_end, binding.buffer_name(), binding.count()));
    }
  }

  // Каждый объявленный `ctx:arg:` обязан найтись в параметрах шага. Молчаливый нуль здесь означал бы
  // правило, которое считает не то, что написано в конфиге, и заметить это было бы почти нечем.
  for (const auto& argument : program.argument_names()) {
    if (!params.has(argument)) {
      return fail(std::format("step '{}': script '{}' reads ctx:arg:{}, but the step declares no such parameter",
                              step_name, name, argument));
    }
  }

  return result;
}

void dispatch_script(const script_program& program,
                     const std::span<const field_ref>& inputs,
                     const std::span<const field_ref>& outputs,
                     const parameters& params,
                     const uint64_t seed,
                     const size_t range_begin,
                     const size_t range_end,
                     const std::string_view& step_name,
                     thread::atomic_pool* pool, const uint32_t output_component, const uint32_t result_component) {
  const auto& impl = *program.impl_;
  const auto evaluated_component = result_component == UINT32_MAX ? output_component : result_component;

  const auto check = check_script_dispatch(program, inputs, outputs, params, range_begin, range_end, step_name, output_component);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  const auto& target = outputs.front();

  std::vector<const_field_accessor> accessors;
  accessors.reserve(inputs.size());
  for (const auto& binding : inputs) {
    accessors.push_back(binding.read());
  }

  const auto output = target.write();
  const bool predicate = impl.kind == script_program::result_kind::predicate;

  const size_t count = range_end > range_begin ? range_end - range_begin : 0;
  if (count == 0) {
    return;
  }

  if (pool == nullptr || pool->size() == 0) {
    run_chunk(impl, accessors, output, predicate, params, seed, range_begin, range_end, output_component, evaluated_component);
    return;
  }

  // Программа pointwise, поэтому границы чанков на результат не влияют: каждый элемент считается
  // независимо, а его случайность выведена из индекса.
  const size_t workers = pool->size() + 1;
  const size_t chunk = std::max<size_t>((count + workers - 1) / workers, 4096);
  for (size_t start = range_begin; start < range_end; start += chunk) {
    const size_t stop = std::min(start + chunk, range_end);
    pool->submit([&impl, &accessors, &output, &params, predicate, seed, output_component, evaluated_component](const size_t begin, const size_t end) {
      run_chunk(impl, accessors, output, predicate, params, seed, begin, end, output_component, evaluated_component);
    }, start, stop);
  }

  pool->compute();
  pool->wait();
}

void check_gather_program(const script_gather_call& call) {
  if (!call.program || !call.params || call.outputs.empty() || call.outputs.size() > 4 ||
      call.reductions.size() != call.outputs.size() || call.own_inputs > call.inputs.size())
    utils::error{}("originator step '{}': invalid scripted gather declaration", call.step);
  const auto& impl = *call.program->impl_;
  if (call.inputs.size() != impl.input_names.size() || call.begin > call.end)
    utils::error{}("originator step '{}': gather inputs/range do not match the program", call.step);
  for (size_t i = 0; i < call.inputs.size(); ++i) {
    const auto& input = call.inputs[i];
    const auto required = i < call.own_inputs ? call.end : call.other_count;
    if (!input.valid() || input.count() < required)
      utils::error{}("originator step '{}': gather input {} does not cover {} elements", call.step, i, required);
    if (!impl.input_types.empty() && input.type().components != impl.input_types[i].components)
      utils::error{}("originator step '{}': gather input {} changed component count", call.step, i);
  }
  for (const auto& name : impl.argument_names)
    if (!call.params->has(name)) utils::error{}("originator step '{}': gather needs parameter '{}'", call.step, name);
  if (call.around) std::visit([&](const auto& around) {
    using topology = std::decay_t<decltype(around)>;
    if constexpr (std::is_same_v<topology, csr_neighbourhood>) {
      if (!around.offsets.valid() || !around.arcs.valid() || around.offsets.count() <= call.end ||
          around.count > call.other_count || (around.weights.valid() && around.weights.count() < around.arcs.count()))
        utils::error{}("originator step '{}': gather CSR does not cover its own/other domains", call.step);
    } else {
      const auto extent = around.extent;
      const auto y = std::max<size_t>(extent.y, 1), z = std::max<size_t>(extent.z, 1);
      if (extent.x == 0 || y > SIZE_MAX / extent.x || z > SIZE_MAX / (extent.x * y) ||
          extent.x * y * z < call.end || extent.x * y * z > call.other_count)
        utils::error{}("originator step '{}': gather stencil does not cover its own/other domains", call.step);
    }
  }, *call.around);
  if (call.validity_component < -1 || call.validity_component > 3)
    utils::error{}("originator step '{}': gather validity component is outside [0,3]", call.step);
  for (const auto& reduction : call.reductions)
    if (reduction.score_component > 3 || std::isnan(reduction.initial))
      utils::error{}("originator step '{}': invalid gather reduction score/initial value", call.step);
  for (size_t a = 0; a < call.outputs.size(); ++a)
    for (size_t b = a + 1; b < call.outputs.size(); ++b)
      if (call.outputs[a].same_field_as(call.outputs[b]))
        utils::error{}("originator step '{}': gather outputs alias each other", call.step);
  for (const auto& output : call.outputs) {
    if (!output.writable() || output.type().components != 1 || output.count() < call.end)
      utils::error{}("originator step '{}': gather needs scalar writable outputs covering its range", call.step);
    for (size_t i = call.own_inputs; i < call.inputs.size(); ++i)
      if (output.same_field_as(call.inputs[i]))
        utils::error{}("originator step '{}': gather cannot modify a field read from neighbours", call.step);
  }
  if (call.empty_self && call.other_count < call.end)
    utils::error{}("originator step '{}': self fallback is outside the other domain", call.step);
}

void dispatch_gather_program(const script_gather_call& call, thread::atomic_pool* pool) {
  check_gather_program(call);
  const auto& impl = *call.program->impl_;
  std::vector<const_field_accessor> inputs;
  for (const auto& input : call.inputs) inputs.push_back(input.read());
  const auto run = [&](const size_t begin, const size_t end) {
    program_frame frame(impl, inputs, *call.params, call.own_inputs);
    for (size_t node = begin; node < end; ++node) {
      std::array<double, 4> values{}, best{};
      std::array<bool, 4> selected{};
      for (size_t c = 0; c < call.outputs.size(); ++c) {
        best[c] = call.reductions[c].initial;
      }
      const auto visit = [&](const size_t other, const double) {
        if (other >= call.other_count) return;
        const auto seed = utils::mix(call.seed, node, other);
        if (call.validity_component >= 0 && frame.evaluate(node, other, uint32_t(call.validity_component), seed) == 0) return;
        for (uint32_t c = 0; c < call.outputs.size(); ++c) {
          const auto& reduction = call.reductions[c];
          const double score = frame.evaluate(node, other, reduction.score_component, seed);
          if (!std::isfinite(score)) utils::error{}("originator step '{}': gather score is not finite at {} -> {}", call.step, node, other);
          using kind = script_reduction::kind;
          if (reduction.operation == kind::sum) {
            selected[c] = true;
            values[c] += score;
            continue;
          }
          const double order = reduction.operation == kind::maximum_absolute ? std::abs(score) : score;
          const bool minimum = reduction.operation == kind::minimum || reduction.operation == kind::argmin;
          if (!(minimum ? order < best[c] : order > best[c])) continue;
          selected[c] = true;
          best[c] = order;
          values[c] = reduction.operation == kind::argmin || reduction.operation == kind::argmax ? double(other) :
            reduction.score_component == c ? score : frame.evaluate(node, other, c, seed);
        }
      };
      if (call.around) std::visit([&](const auto& around) { around.each(node, visit); }, *call.around);
      else for (size_t other = 0; other < call.other_count; ++other) visit(other, 1.0);
      for (uint32_t c = 0; c < call.outputs.size(); ++c) {
        if (!selected[c] && call.empty_self) {
          const auto op = call.reductions[c].operation;
          values[c] = op == script_reduction::kind::argmin || op == script_reduction::kind::argmax ? double(node) :
            frame.evaluate(node, node, c, utils::mix(call.seed, node, node));
        }
      }
      for (size_t c = 0; c < call.outputs.size(); ++c) call.outputs[c].write().set(node, values[c]);
    }
  };
  if (pool && pool->size()) {
    const size_t chunk = std::max<size_t>(4096, (call.end - call.begin + pool->size()) / (pool->size() + 1));
    for (size_t begin = call.begin; begin < call.end; begin += chunk) {
      const size_t end = std::min(call.end, begin + chunk);
      pool->submit([&run](size_t first, size_t last) { run(first, last); }, begin, end);
    }
    pool->compute(); pool->wait();
  } else run(call.begin, call.end);
}

} // namespace originator
} // namespace devils_engine
