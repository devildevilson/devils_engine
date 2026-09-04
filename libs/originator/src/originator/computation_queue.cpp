#include "devils_engine/originator/computation_queue.h"

#include <format>

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
bool reads_field(const queue_call& call, const field_ref& field) noexcept {
  for (const auto& binding : call.inputs) {
    if (binding.same_field_as(field)) return true;
  }
  return false;
}

// Затирает ли этот вызов поле ЦЕЛИКОМ в границах прежней записи. Покрытие проверяется не ради
// строгости, а чтобы не объявить мёртвым честный случай: два вызова, пишущие РАЗНЫЕ отрезки одного
// поля, оба живые, и ложный отказ здесь стоил бы дороже пропущенного предупреждения.
bool overwrites_field(const queue_call& call, const field_ref& field, const size_t begin, const size_t end) noexcept {
  for (const auto& binding : call.outputs) {
    if (!binding.same_field_as(field)) continue;
    if (call.range_begin <= begin && call.range_end >= end) return true;
  }
  return false;
}

bool named_in_output(const computation_queue& queue, const field_ref& field) noexcept {
  for (const auto& binding : queue.output) {
    if (binding.same_field_as(field)) return true;
  }
  return false;
}
} // namespace

bool fits_in_queue(const aperture::values value) noexcept {
  return value == aperture::pointwise || value == aperture::gather;
}

std::string_view queue_rejection_reason(const aperture::values value) noexcept {
  switch (value) {
    case aperture::scatter:
      return "scatter writes by foreign indices into a structure of another shape, and its order comes from the "
             "algorithm rather than from the queue: call such a tool on its own";
    case aperture::sequential:
      return "sequential work is ordered by construction, so the queue breaks on it — and that break is a "
             "boundary worth seeing in the config";
    case aperture::reduce:
      return "a reduction yields ONE number and the queue keeps no intermediate values in lua, so there is "
             "nowhere to put it: read the reduction outside the queue, which is exactly what it is for";
    default:
      return "the aperture does not write its own element into a declared output";
  }
}

queue_check check_queue(const computation_queue& queue) {
  queue_check result;

  const auto fail = [&](std::string message) {
    result.allowed = false;
    result.message = std::move(message);
    return result;
  };

  if (queue.calls.empty()) {
    return fail(std::format("step '{}': the queue declares no calls", queue.name));
  }

  // Пустой выход — отказ, а не мелочь: очередь существует ради того, что она отдаёт наружу, и без
  // объявленной границы передачи проверить мёртвую работу тоже нечем.
  if (queue.output.empty()) {
    return fail(std::format("step '{}': the queue declares no output, so nothing it computes leaves it — "
                            "name the fields the rest of the step reads in 'output'",
                            queue.name));
  }

  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];
    const size_t position = i + 1;

    if (call.tool == nullptr && !call.body) {
      return fail(std::format("step '{}': queue element {} ('{}') has neither a tool nor a body",
                              queue.name, position, call.label));
    }

    if (!fits_in_queue(call.shape)) {
      return fail(std::format("step '{}': queue element {} ('{}') has aperture '{}', which the queue does not take: {}",
                              queue.name, position, call.label, to_string(call.shape), queue_rejection_reason(call.shape)));
    }

    if (call.outputs.empty()) {
      return fail(std::format("step '{}': queue element {} ('{}') writes nothing",
                              queue.name, position, call.label));
    }

    // Привязки нативного инструмента проверяются ровно тем же, чем проверяется одиночный вызов:
    // второго набора правил у очереди нет и быть не должно. Чужое тело проверяет тот, кто его
    // собрал, — только он знает, против чего скомпилирована программа.
    if (call.tool != nullptr) {
      const auto check = check_dispatch(*call.tool, call.inputs, call.outputs, call.range_begin, call.range_end, queue.name);
      if (!check.allowed) {
        return fail(std::format("{}; that call is queue element {} of {}", check.message, position, queue.calls.size()));
      }
    }
  }

  for (const auto& binding : queue.output) {
    if (!binding.valid()) {
      return fail(std::format("step '{}': the queue names an invalid field in 'output'", queue.name));
    }

    bool produced = false;
    for (const auto& call : queue.calls) {
      for (const auto& written : call.outputs) {
        produced = produced || written.same_field_as(binding);
      }
    }
    if (!produced) {
      return fail(std::format("step '{}': the queue names '{}.{}' in 'output', but no call inside writes it",
                              queue.name, binding.buffer_name(), binding.field_name()));
    }
  }

  // МЁРТВАЯ РАБОТА. Проход считается впустую, если его результат никто дальше не читает и он не
  // назван границей передачи, — либо потому, что его позже затёрли, не прочитав.
  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];

    bool alive = false;
    for (const auto& written : call.outputs) {
      bool read_later = false;
      bool covered_later = false;
      for (size_t j = i + 1; j < queue.calls.size(); ++j) {
        read_later = read_later || reads_field(queue.calls[j], written);
        covered_later = covered_later || overwrites_field(queue.calls[j], written, call.range_begin, call.range_end);
      }

      alive = alive || read_later || (!covered_later && named_in_output(queue, written));
    }

    if (!alive) {
      const auto& written = call.outputs.front();
      return fail(std::format("step '{}': queue element {} ('{}') writes '{}.{}', but nothing inside the queue reads "
                              "it and 'output' does not name it — the pass is dead",
                              queue.name, i + 1, call.label, written.buffer_name(), written.field_name()));
    }
  }

  return result;
}

queue_report run_queue(const computation_queue& queue, thread::atomic_pool* pool) {
  const auto check = check_queue(queue);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  // Слияние соседних проходов встанет ЗДЕСЬ: два подряд pointwise над одним диапазоном — это один
  // обход данных, и данные останутся в кэше. Пока обходов ровно столько, сколько элементов, и это
  // честная точка отсчёта для замера.
  queue_report report;
  report.calls = queue.calls.size();

  for (const auto& call : queue.calls) {
    if (call.tool != nullptr) {
      dispatch(*call.tool, call.inputs, call.outputs, call.params, call.seed, call.range_begin, call.range_end,
               queue.name, pool);
    } else {
      call.body(call, queue.name, pool);
    }
    ++report.passes;
  }

  return report;
}

} // namespace originator
} // namespace devils_engine
