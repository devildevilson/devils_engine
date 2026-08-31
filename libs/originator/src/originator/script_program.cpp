#include "devils_engine/originator/script_program.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>

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

struct script_program::implementation {
  std::string name;
  std::vector<std::string> input_names;
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

  return program;
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
               const uint64_t seed,
               const size_t begin,
               const size_t end) {
  // Контекст devils_script — изменяемое состояние, поэтому он СВОЙ у каждого чанка. Размеры берутся
  // из скомпилированной программы, а не из умолчаний.
  ds::context vm(std::max<size_t>(impl.program.max_stack, 1), std::max<size_t>(impl.program.max_saved, 1));
  vm.create_lists(&impl.program);

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

void dispatch_script(const script_program& program,
                     const std::span<const field_ref>& inputs,
                     const std::span<const field_ref>& outputs,
                     const uint64_t seed,
                     const size_t range_begin,
                     const size_t range_end,
                     const std::string_view& step_name,
                     thread::atomic_pool* pool) {
  const auto& impl = *program.impl_;

  if (inputs.size() != impl.input_names.size()) {
    utils::error{}("originator step '{}': script '{}' was compiled against {} inputs, got {}",
                   step_name, impl.name, impl.input_names.size(), inputs.size());
  }
  if (outputs.size() != 1) {
    utils::error{}("originator step '{}': script '{}' writes exactly one field, got {}",
                   step_name, impl.name, outputs.size());
  }

  // Имя привязки обязано совпасть с тем, под которым поле видно в скрипте: иначе скрипт считал бы
  // одно, а получал другое, и это не заметно ни в конфиге, ни в тексте скрипта.
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (!inputs[i].valid()) {
      utils::error{}("originator step '{}': script '{}' got an invalid input {}", step_name, impl.name, i);
    }
    if (inputs[i].field_name() != impl.input_names[i]) {
      utils::error{}("originator step '{}': script '{}' reads input {} as '{}', but the binding is '{}.{}'",
                     step_name, impl.name, i, impl.input_names[i], inputs[i].buffer_name(), inputs[i].field_name());
    }
  }

  const auto& target = outputs.front();
  if (!target.valid()) {
    utils::error{}("originator step '{}': script '{}' got an invalid output", step_name, impl.name);
  }
  if (!target.writable()) {
    utils::error{}("originator step '{}': script '{}' writes to '{}.{}', but that buffer is bound for reading",
                   step_name, impl.name, target.buffer_name(), target.field_name());
  }
  if (range_end < range_begin) {
    utils::error{}("originator step '{}': script '{}' got an inverted range [{}, {})",
                   step_name, impl.name, range_begin, range_end);
  }
  if (range_end > target.count()) {
    utils::error{}("originator step '{}': script '{}' range [{}, {}) exceeds '{}' with {} elements",
                   step_name, impl.name, range_begin, range_end, target.buffer_name(), target.count());
  }
  for (const auto& binding : inputs) {
    if (range_end > binding.count()) {
      utils::error{}("originator step '{}': script '{}' range [{}, {}) exceeds '{}' with {} elements",
                     step_name, impl.name, range_begin, range_end, binding.buffer_name(), binding.count());
    }
  }

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
    run_chunk(impl, accessors, output, predicate, seed, range_begin, range_end);
    return;
  }

  // Программа pointwise, поэтому границы чанков на результат не влияют: каждый элемент считается
  // независимо, а его случайность выведена из индекса.
  const size_t workers = pool->size() + 1;
  const size_t chunk = std::max<size_t>((count + workers - 1) / workers, 4096);
  for (size_t start = range_begin; start < range_end; start += chunk) {
    const size_t stop = std::min(start + chunk, range_end);
    pool->submit([&impl, &accessors, &output, predicate, seed](const size_t begin, const size_t end) {
      run_chunk(impl, accessors, output, predicate, seed, begin, end);
    }, start, stop);
  }

  pool->compute();
  pool->wait();
}

} // namespace originator
} // namespace devils_engine
