#include "devils_engine/originator/tools.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

bool field_ref::valid() const noexcept {
  return source != nullptr && field_index < source->layout().fields.size();
}

bool field_ref::writable() const noexcept {
  return target != nullptr && valid();
}

size_t field_ref::count() const noexcept {
  return source != nullptr ? source->count() : 0;
}

field_type field_ref::type() const noexcept {
  return valid() ? source->layout().fields[field_index].type : field_type{};
}

std::string_view field_ref::buffer_name() const noexcept {
  return source != nullptr ? std::string_view(source->name()) : std::string_view("<none>");
}

std::string_view field_ref::field_name() const noexcept {
  return valid() ? std::string_view(source->layout().fields[field_index].name) : std::string_view("<none>");
}

const_field_accessor field_ref::read() const noexcept {
  return valid() ? source->field(field_index) : const_field_accessor{};
}

field_accessor field_ref::write() const noexcept {
  return writable() ? target->field(field_index) : field_accessor{};
}

bool field_ref::same_field_as(const field_ref& other) const noexcept {
  return source == other.source && field_index == other.field_index;
}

void parameters::set_number(const std::string_view& name, const double value) {
  for (auto& entry : entries_) {
    if (entry.name == name) {
      entry.value = value;
      entry.is_string = false;
      entry.text.clear();
      return;
    }
  }
  entries_.push_back(entry{std::string(name), std::string{}, value, false});
}

void parameters::set_string(const std::string_view& name, std::string value) {
  for (auto& entry : entries_) {
    if (entry.name == name) {
      entry.text = std::move(value);
      entry.is_string = true;
      return;
    }
  }
  entries_.push_back(entry{std::string(name), std::move(value), 0.0, true});
}

const parameters::entry* parameters::find(const std::string_view& name) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool parameters::has(const std::string_view& name) const noexcept {
  return find(name) != nullptr;
}

size_t parameters::size() const noexcept {
  return entries_.size();
}

std::string_view parameters::name_at(const size_t index) const noexcept {
  return index < entries_.size() ? std::string_view(entries_[index].name) : std::string_view{};
}

bool parameters::is_string_at(const size_t index) const noexcept {
  return index < entries_.size() && entries_[index].is_string;
}

double parameters::number_at(const size_t index) const noexcept {
  return index < entries_.size() ? entries_[index].value : 0.0;
}

std::string_view parameters::string_at(const size_t index) const noexcept {
  return index < entries_.size() ? std::string_view(entries_[index].text) : std::string_view{};
}

void parameters::overlay(const parameters& other) {
  for (size_t i = 0; i < other.size(); ++i) {
    const auto name = other.name_at(i);
    if (other.is_string_at(i)) {
      set_string(name, std::string(other.string_at(i)));
    } else {
      set_number(name, other.number_at(i));
    }
  }
}

double parameters::number(const std::string_view& name, const double fallback) const noexcept {
  const auto* entry = find(name);
  return entry != nullptr && !entry->is_string ? entry->value : fallback;
}

int64_t parameters::integer(const std::string_view& name, const int64_t fallback) const noexcept {
  const auto* entry = find(name);
  return entry != nullptr && !entry->is_string ? int64_t(entry->value) : fallback;
}

std::string_view parameters::string(const std::string_view& name, const std::string_view& fallback) const noexcept {
  const auto* entry = find(name);
  return entry != nullptr && entry->is_string ? std::string_view(entry->text) : fallback;
}

size_t tool_call::range_count() const noexcept {
  return range_end > range_begin ? range_end - range_begin : 0;
}

const field_ref& tool_call::input(const size_t index) const {
  if (index >= inputs.size()) {
    utils::error{}("originator step '{}': tool '{}' requested input {} of {}", step_name, tool_name, index, inputs.size());
  }
  return inputs[index];
}

const field_ref& tool_call::output(const size_t index) const {
  if (index >= outputs.size()) {
    utils::error{}("originator step '{}': tool '{}' requested output {} of {}", step_name, tool_name, index, outputs.size());
  }
  return outputs[index];
}

void tool_registry::add(tool_description description) {
  if (description.name.empty()) {
    utils::error{}("originator: tool must have a name");
  }
  if (find(description.name) != nullptr) {
    utils::error{}("originator: tool '{}' is already registered", description.name);
  }

  const bool is_reduce = description.shape == aperture::reduce;
  if (is_reduce && (description.partial == nullptr || description.combine == nullptr)) {
    utils::error{}("originator: reduce tool '{}' must provide both partial and combine", description.name);
  }
  if (!is_reduce && description.body == nullptr) {
    utils::error{}("originator: tool '{}' has no body", description.name);
  }

  tools_.push_back(std::move(description));
}

const tool_description* tool_registry::find(const std::string_view& name) const noexcept {
  for (const auto& tool : tools_) {
    if (tool.name == name) {
      return &tool;
    }
  }
  return nullptr;
}

const tool_description& tool_registry::at(const size_t index) const noexcept {
  return tools_[index];
}

size_t tool_registry::size() const noexcept {
  return tools_.size();
}

dispatch_check check_dispatch(const tool_description& tool,
                              const std::span<const field_ref>& inputs,
                              const std::span<const field_ref>& outputs,
                              const size_t range_begin,
                              const size_t range_end,
                              const std::string_view& step_name) {
  dispatch_check result;
  result.parallel = is_parallel(tool.shape);

  const auto fail = [&](std::string message) {
    result.allowed = false;
    result.parallel = false;
    result.message = std::move(message);
    return result;
  };

  if (inputs.size() != tool.input_count) {
    return fail(std::format("step '{}': tool '{}' expects {} inputs, got {}", step_name, tool.name, tool.input_count, inputs.size()));
  }
  if (outputs.size() != tool.output_count) {
    return fail(std::format("step '{}': tool '{}' expects {} outputs, got {}", step_name, tool.name, tool.output_count, outputs.size()));
  }

  for (size_t i = 0; i < inputs.size(); ++i) {
    if (!inputs[i].valid()) {
      return fail(std::format("step '{}': tool '{}' got an invalid input {}", step_name, tool.name, i));
    }
  }

  for (size_t i = 0; i < outputs.size(); ++i) {
    if (!outputs[i].valid()) {
      return fail(std::format("step '{}': tool '{}' got an invalid output {}", step_name, tool.name, i));
    }
    if (!outputs[i].writable()) {
      return fail(std::format("step '{}': tool '{}' writes to '{}.{}', but that buffer is bound for reading",
                              step_name, tool.name, outputs[i].buffer_name(), outputs[i].field_name()));
    }
  }

  if (range_end < range_begin) {
    return fail(std::format("step '{}': tool '{}' got an inverted range [{}, {})", step_name, tool.name, range_begin, range_end));
  }

  // К чему относится диапазон, зависит от апертуры, и это не косметика:
  //   pointwise/reduce — и к входам, и к выходам: элемент i читается и пишется по одному индексу;
  //   gather — только к выходам: входы читаются целиком, там и лежат соседи;
  //   scatter — только к входам: выход это ДРУГАЯ структура (смещения групп, суммы по корзинам),
  //             и её размер задаёт число групп, а не число обрабатываемых элементов.
  const bool range_covers_outputs = tool.shape != aperture::scatter;
  const bool range_covers_inputs = tool.shape != aperture::gather;

  if (range_covers_outputs) {
    for (const auto& binding : outputs) {
      if (range_end > binding.count()) {
        return fail(std::format("step '{}': tool '{}' range [{}, {}) exceeds '{}' with {} elements",
                                step_name, tool.name, range_begin, range_end, binding.buffer_name(), binding.count()));
      }
    }
  }
  if (range_covers_inputs) {
    for (const auto& binding : inputs) {
      if (range_end > binding.count()) {
        return fail(std::format("step '{}': tool '{}' range [{}, {}) exceeds '{}' with {} elements",
                                step_name, tool.name, range_begin, range_end, binding.buffer_name(), binding.count()));
      }
    }
  }

  // Главная проверка апертуры. gather читает соседей, поэтому источник обязан быть неизменным на
  // всё время прохода — а он неизменен ровно тогда, когда это НЕ то же самое поле, что приёмник.
  if (tool.shape == aperture::gather) {
    for (const auto& out : outputs) {
      for (const auto& in : inputs) {
        if (out.same_field_as(in)) {
          return fail(std::format(
            "step '{}': tool '{}' has a gather aperture and cannot read and write the same field '{}.{}' — "
            "give it a separate destination field, otherwise neighbours would be read in an undefined state",
            step_name, tool.name, out.buffer_name(), out.field_name()));
        }
      }
    }
  }

  return result;
}

dispatch_check check_key_support(const tool_description& tool,
                                 const key_support::values support,
                                 const bool chunk_active,
                                 const std::string_view& step_name) {
  dispatch_check result;
  result.parallel = is_parallel(tool.shape);

  if (tool.shape != aperture::scatter || !chunk_active) {
    return result;
  }

  if (support == key_support::chunk_local) {
    return result;
  }

  result.allowed = false;
  result.parallel = false;
  result.message = std::format(
    "step '{}': tool '{}' scatters into groups declared as '{}', but the pipeline is generating a chunk. "
    "A global group is never finished by one chunk, so the result would depend on which chunks ran — "
    "compute such a summary in the coarse world pass, or declare key_support = chunk_local if the group "
    "really does fit inside one chunk",
    step_name, tool.name, to_string(support == key_support::count ? key_support::global : support));
  return result;
}

namespace {
// Разбиение параллельной работы. Для pointwise/gather элементы независимы, поэтому границы чанков
// на результат не влияют; фиксированный минимальный размер нужен только чтобы не платить за
// диспетчеризацию больше, чем за саму работу.
constexpr size_t minimum_parallel_chunk = 4096;

size_t chunk_size_for(const size_t count, const size_t worker_count) noexcept {
  if (worker_count <= 1) {
    return count;
  }
  const size_t even = (count + worker_count - 1) / worker_count;
  return std::max(even, minimum_parallel_chunk);
}
} // namespace

void dispatch(const tool_description& tool,
              const std::span<const field_ref>& inputs,
              const std::span<const field_ref>& outputs,
              const parameters& params,
              const uint64_t seed,
              const size_t range_begin,
              const size_t range_end,
              const std::string_view& step_name,
              thread::atomic_pool* pool) {
  if (tool.shape == aperture::reduce) {
    utils::error{}("originator step '{}': tool '{}' is a reduction, use dispatch_reduce", step_name, tool.name);
  }

  const auto check = check_dispatch(tool, inputs, outputs, range_begin, range_end, step_name);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  tool_call call;
  call.inputs = inputs;
  call.outputs = outputs;
  call.params = &params;
  call.seed = seed;
  call.range_begin = range_begin;
  call.range_end = range_end;
  call.step_name = step_name;
  call.tool_name = tool.name;
  // Пул отдаётся только тем апертурам, которые движок не разбивает сам.
  call.pool = check.parallel ? nullptr : pool;

  const size_t count = call.range_count();
  if (count == 0) {
    return;
  }

  // Подготовка идёт до разбиения: её результат общий для всех чанков и неизменен на всё время вызова.
  std::shared_ptr<void> shared;
  if (tool.prepare != nullptr) {
    shared = tool.prepare(call);
    call.shared = shared.get();
  }

  if (!check.parallel || pool == nullptr || pool->size() == 0) {
    tool.body(call, range_begin, range_end);
    return;
  }

  const size_t chunk = chunk_size_for(count, pool->size() + 1);
  for (size_t start = range_begin; start < range_end; start += chunk) {
    const size_t stop = std::min(start + chunk, range_end);
    pool->submit([&tool, &call](const size_t begin, const size_t end) { tool.body(call, begin, end); }, start, stop);
  }

  pool->compute();
  pool->wait();
}

double dispatch_reduce(const tool_description& tool,
                       const std::span<const field_ref>& inputs,
                       const parameters& params,
                       const uint64_t seed,
                       const size_t range_begin,
                       const size_t range_end,
                       const std::string_view& step_name,
                       thread::atomic_pool* pool) {
  if (tool.shape != aperture::reduce) {
    utils::error{}("originator step '{}': tool '{}' is not a reduction", step_name, tool.name);
  }

  const std::span<const field_ref> no_outputs;
  const auto check = check_dispatch(tool, inputs, no_outputs, range_begin, range_end, step_name);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  tool_call call;
  call.inputs = inputs;
  call.outputs = no_outputs;
  call.params = &params;
  call.seed = seed;
  call.range_begin = range_begin;
  call.range_end = range_end;
  call.step_name = step_name;
  call.tool_name = tool.name;

  const size_t count = call.range_count();
  if (count == 0) {
    return tool.initial;
  }

  // Число чанков и их границы заданы КОНСТАНТОЙ, а не числом потоков: объединение идёт по порядку
  // чанков, поэтому результат один и тот же при любом раскладе исполнения. Чанки при этом
  // группируются в задачи — очередь пула ограничена, и класть в неё сотни тысяч задач нельзя.
  const size_t chunk_count = (count + reduce_chunk_size - 1) / reduce_chunk_size;
  std::vector<double> partials(chunk_count, tool.initial);

  const auto run_chunks = [&tool, &call, &partials, range_begin, range_end](const size_t first, const size_t last) {
    for (size_t i = first; i < last; ++i) {
      const size_t start = range_begin + i * reduce_chunk_size;
      const size_t stop = std::min(start + reduce_chunk_size, range_end);
      partials[i] = tool.partial(call, start, stop);
    }
  };

  if (pool == nullptr || pool->size() == 0 || chunk_count == 1) {
    run_chunks(0, chunk_count);
  } else {
    const size_t task_count = std::min(chunk_count, (pool->size() + 1) * 4);
    const size_t chunks_per_task = (chunk_count + task_count - 1) / task_count;
    for (size_t first = 0; first < chunk_count; first += chunks_per_task) {
      const size_t last = std::min(first + chunks_per_task, chunk_count);
      pool->submit(run_chunks, first, last);
    }
    pool->compute();
    pool->wait();
  }

  double accumulated = tool.initial;
  for (const double partial : partials) {
    accumulated = tool.combine(accumulated, partial);
  }
  return accumulated;
}

} // namespace originator
} // namespace devils_engine
