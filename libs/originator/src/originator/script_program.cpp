#include "devils_engine/originator/script_program.h"

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

namespace devils_engine {
namespace originator {

namespace {
namespace ds = devils_script;

// Скоуп скрипта — ОДИН элемент. Влезает в 16 байт стека devils_script, тривиально разрушается.
// Больше в нём ничего нет намеренно: из этой пары нельзя добраться ни до соседа, ни до записи.
struct script_element {
  const const_field_accessor* accessors = nullptr;
  uint64_t index = 0;

  bool valid() const noexcept {
    return accessors != nullptr;
  }
};

static_assert(sizeof(script_element) <= 16, "devils_script scope must fit the VM stack element");
static_assert(std::is_trivially_destructible_v<script_element>);

// Аксессор k-го входного поля. Индекс элемента ВСЕГДА берётся из скоупа, поэтому обращение к
// чужому элементу не выражается на уровне регистрации.
template <size_t index>
double read_input(const script_element element) {
  return element.accessors[index].get(element.index);
}

template <size_t... indices>
void register_readers(ds::system& system, const std::span<const std::string>& names, std::index_sequence<indices...>) {
  ((indices < names.size() ? void(system.register_function<&read_input<indices>>(names[indices])) : void()), ...);
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
                                                        const result_kind kind) {
  if (input_names.size() > max_script_inputs) {
    utils::error{}("originator script '{}': {} input fields, at most {} are supported",
                   name, input_names.size(), max_script_inputs);
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
  impl.kind = kind;

  impl.system.init_basic_functions();
  impl.system.init_math();
  register_readers(impl.system, input_names, std::make_index_sequence<max_script_inputs>{});

  // Небезопасные варианты опкодов. Проверка типов остаётся ГДЕ ЕЙ МЕСТО — на разборе: программа,
  // которая не сошлась по типам, до исполнения не доходит вообще. Платить за повторную проверку
  // типа стека на каждом из миллионов элементов незачем.
  impl.system.toggle_safety();

  try {
    impl.program = kind == result_kind::predicate
                     ? impl.system.parse<bool, script_element>(name, source)
                     : impl.system.parse<double, script_element>(name, source);
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

  return program;
}

const std::vector<std::string>& script_program::argument_names() const noexcept {
  return impl_->argument_names;
}

const std::string& script_program::name() const noexcept {
  return impl_->name;
}

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

void run_chunk(const script_program::implementation& impl,
               const std::span<const const_field_accessor>& accessors,
               const field_accessor& output,
               const bool predicate,
               const parameters& params,
               const uint64_t seed,
               const size_t begin,
               const size_t end) {
  // Контекст devils_script — изменяемое состояние, поэтому он СВОЙ у каждого чанка. Размеры берутся
  // из скомпилированной программы, а не из умолчаний.
  ds::context vm(std::max<size_t>(impl.program.max_stack, 1), std::max<size_t>(impl.program.max_saved, 1));
  vm.create_lists(&impl.program);

  // Аргументы постоянны на весь проход, поэтому выставляются ОДИН раз: clear() их не сбрасывает,
  // он трогает только операндный стек.
  for (const auto& argument : impl.arguments) {
    switch (argument.value_kind) {
      case script_argument::kind::number: vm.set_arg(argument.slot, params.number(argument.name)); break;
      case script_argument::kind::integer: vm.set_arg(argument.slot, params.integer(argument.name)); break;
      case script_argument::kind::boolean: vm.set_arg(argument.slot, params.number(argument.name) != 0.0); break;
    }
  }

  script_element element{accessors.data(), 0};

  for (size_t i = begin; i < end; ++i) {
    element.index = i;

    vm.clear();
    // Случайность внутри скрипта — функция от элемента, а не от порядка обхода.
    vm.prng_state = utils::mix(seed, i);
    vm.set_arg(0, element);
    impl.program.process(&vm);

    const double value = predicate ? (vm.get_return<bool>() ? 1.0 : 0.0) : vm.get_return<double>();
    output.set(i, value);
  }
}

dispatch_check check_script_dispatch(const script_program& program,
                                    const std::span<const field_ref>& inputs,
                                    const std::span<const field_ref>& outputs,
                                    const parameters& params,
                                    const size_t range_begin,
                                    const size_t range_end,
                                    const std::string_view& step_name) {
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
                     thread::atomic_pool* pool) {
  const auto& impl = *program.impl_;

  const auto check = check_script_dispatch(program, inputs, outputs, params, range_begin, range_end, step_name);
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
    run_chunk(impl, accessors, output, predicate, params, seed, range_begin, range_end);
    return;
  }

  // Программа pointwise, поэтому границы чанков на результат не влияют: каждый элемент считается
  // независимо, а его случайность выведена из индекса.
  const size_t workers = pool->size() + 1;
  const size_t chunk = std::max<size_t>((count + workers - 1) / workers, 4096);
  for (size_t start = range_begin; start < range_end; start += chunk) {
    const size_t stop = std::min(start + chunk, range_end);
    pool->submit([&impl, &accessors, &output, &params, predicate, seed](const size_t begin, const size_t end) {
      run_chunk(impl, accessors, output, predicate, params, seed, begin, end);
    }, start, stop);
  }

  pool->compute();
  pool->wait();
}

} // namespace originator
} // namespace devils_engine
