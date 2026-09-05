#include "devils_engine/originator/computation_queue.h"

#include <algorithm>
#include <format>
#include <span>
#include <vector>

#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

size_t queue_call::range_count() const noexcept {
  return range_end > range_begin ? range_end - range_begin : 0;
}

bool queue_call::indirect() const noexcept {
  return count_from.valid();
}

bool queue_call::reads(const field_ref& field) const noexcept {
  for (const auto& binding : inputs) {
    if (binding.same_field_as(field)) return true;
  }
  return count_from.valid() && count_from.same_field_as(field);
}

bool queue_call::writes(const field_ref& field) const noexcept {
  for (const auto& binding : outputs) {
    if (binding.same_field_as(field)) return true;
  }
  return false;
}

namespace {
// Объявил ли вызов свои записи независимыми от порядка. У нативного инструмента это свойство самого
// инструмента, у чужого тела — того, кто его собрал: тот же порядок, что у устройственной формы.
bool order_free(const queue_call& call) noexcept {
  return call.tool != nullptr ? call.tool->order_free_writes : call.order_free_writes;
}

// НАКОПИТЕЛЬ ЧИТАЕТ ТО, ВО ЧТО ПИШЕТ, и это меняет ответы обеих проверок сразу.
//
// `rain[i] += ...` — не запись поля, а его изменение, поэтому проход, заполнивший `rain` нулями
// ДО накопителя, живой (иначе очередь объявила бы мёртвой честную инициализацию), а барьер между
// ними нужен. Оба вывода идут отсюда, и это тот же единственный вопрос, а не третий набор правил.
bool reads_for_liveness(const queue_call& call, const field_ref& field) noexcept {
  return call.reads(field) || (order_free(call) && call.writes(field));
}

// Затирает ли этот вызов поле ЦЕЛИКОМ в границах прежней записи. Покрытие проверяется не ради
// строгости, а чтобы не объявить мёртвым честный случай: два вызова, пишущие РАЗНЫЕ отрезки одного
// поля, оба живые, и ложный отказ здесь стоил бы дороже пропущенного предупреждения.
//
// Накопитель не затирает ничего по определению: он прибавляет к тому, что нашёл.
bool overwrites_field(const queue_call& call, const field_ref& field, const size_t begin, const size_t end) noexcept {
  return !order_free(call) && call.writes(field) && call.range_begin <= begin && call.range_end >= end;
}

// Назван ли на ГРАНИЦЕ — в любом из двух списков. Проверке мёртвой работы всё равно, где живёт
// читатель: она спрашивает, прочитает ли это хоть кто-нибудь.
bool named_at_boundary(const computation_queue& queue, const field_ref& field) noexcept {
  for (const auto& binding : queue.output) {
    if (binding.same_field_as(field)) return true;
  }
  for (const auto& binding : queue.resident) {
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
      return "scatter writes by foreign indices into a structure of another shape, and the ORDER of those writes "
             "comes from the algorithm rather than from the queue: call such a tool on its own, or — if its writes "
             "genuinely do not depend on order, the way integer accumulation does not — declare that";
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

  // Пустая граница — отказ, а не мелочь: очередь существует ради того, что читают после неё, и без
  // объявленной границы проверить мёртвую работу тоже нечем. Списка два, и хватает любого: `output`
  // называет то, что едет на хост, `resident` — то, что остаётся живым на устройстве.
  if (queue.output.empty() && queue.resident.empty()) {
    return fail(std::format("step '{}': the queue declares no boundary at all, so nothing it computes is read — "
                            "name what the rest of the step reads in 'output', or what stays on the device in "
                            "'resident'",
                            queue.name));
  }

  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];
    const size_t position = i + 1;

    if (call.tool == nullptr && !call.body) {
      return fail(std::format("step '{}': queue element {} ('{}') has neither a tool nor a body",
                              queue.name, position, call.label));
    }

    // АПЕРТУРА. У `scatter` есть объявляемое исключение, и оно проверяется здесь, а не в
    // `fits_in_queue`: независимость записей от порядка — свойство КОНКРЕТНОГО инструмента, а не
    // рода адресации, и спрашивать о ней надо у вызова.
    const bool admitted = fits_in_queue(call.shape) || (call.shape == aperture::scatter && order_free(call));
    if (!admitted) {
      return fail(std::format("step '{}': queue element {} ('{}') has aperture '{}', which the queue does not take: {}",
                              queue.name, position, call.label, to_string(call.shape), queue_rejection_reason(call.shape)));
    }

    if (call.outputs.empty()) {
      return fail(std::format("step '{}': queue element {} ('{}') writes nothing",
                              queue.name, position, call.label));
    }

    if (call.indirect()) {
      if (!valid_count_field(call.count_from)) {
        return fail(std::format("step '{}': queue element {} ('{}') counts elements from '{}.{}', which is not a "
                                "single-component integer — a fractional element count is a silent truncation",
                                queue.name, position, call.label, call.count_from.buffer_name(),
                                call.count_from.field_name()));
      }
      // Счётчик обязан быть УЖЕ посчитан: его пишет либо предыдущий элемент очереди, либо хост до
      // запуска. Запись позже означала бы, что вызов читает значение с прошлого раза — и заметить
      // это можно только по неправдоподобной длине, если она вообще неправдоподобна.
      for (size_t later = i + 1; later < queue.calls.size(); ++later) {
        if (!queue.calls[later].writes(call.count_from)) continue;
        return fail(std::format("step '{}': queue element {} ('{}') counts elements from '{}.{}', but element {} "
                                "writes that field LATER — the count would be the one from the previous run",
                                queue.name, position, call.label, call.count_from.buffer_name(),
                                call.count_from.field_name(), later + 1));
      }
      if (call.range_begin != 0) {
        return fail(std::format("step '{}': queue element {} ('{}') counts elements from a field and cannot also "
                                "start at {} — a counted range starts at zero",
                                queue.name, position, call.label, call.range_begin));
      }
    }

    // Привязки нативного инструмента проверяются ровно тем же, чем проверяется одиночный вызов:
    // второго набора правил у очереди нет и быть не должно. Чужое тело проверяет тот, кто его
    // собрал, — только он знает, против чего скомпилирована программа.
    //
    // У вызова со счётчиком проверяется ЁМКОСТЬ, а не диапазон: сколько элементов он обработает,
    // до исполнения неизвестно, поэтому границей служит объявленный размер приёмника, а превышение
    // зажимается по ней в момент вызова.
    if (call.tool != nullptr) {
      const size_t checked_end = call.indirect() ? call.outputs.front().count() : call.range_end;
      const auto check =
        check_dispatch(*call.tool, call.inputs, call.outputs, call.range_begin, checked_end, queue.name);
      if (!check.allowed) {
        return fail(std::format("{}; that call is queue element {} of {}", check.message, position, queue.calls.size()));
      }
    }
  }

  const auto check_boundary = [&](const std::vector<field_ref>& list, const std::string_view& which) -> std::string {
    for (const auto& binding : list) {
      if (!binding.valid()) {
        return std::format("step '{}': the queue names an invalid field in '{}'", queue.name, which);
      }

      bool produced = false;
      for (const auto& call : queue.calls) {
        for (const auto& written : call.outputs) {
          produced = produced || written.same_field_as(binding);
        }
      }
      if (!produced) {
        return std::format("step '{}': the queue names '{}.{}' in '{}', but no call inside writes it",
                           queue.name, binding.buffer_name(), binding.field_name(), which);
      }
    }
    return {};
  };

  const auto output_message = check_boundary(queue.output, "output");
  if (!output_message.empty()) return fail(output_message);
  const auto resident_message = check_boundary(queue.resident, "resident");
  if (!resident_message.empty()) return fail(resident_message);

  // Одно поле в обоих списках — не ошибка исполнения, а ДВА ответа на один вопрос: оно и едет, и
  // остаётся. Один из двух окажется неправдой, и какой именно — по результату не видно.
  for (const auto& binding : queue.resident) {
    for (const auto& leaving : queue.output) {
      if (!binding.same_field_as(leaving)) continue;
      return fail(std::format("step '{}': '{}.{}' is named both in 'output' and in 'resident' — it either comes "
                              "back to the host or it does not, and two answers here mean one of them is untrue",
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
        read_later = read_later || reads_for_liveness(queue.calls[j], written);
        covered_later = covered_later || overwrites_field(queue.calls[j], written, call.range_begin, call.range_end);
      }

      alive = alive || read_later || (!covered_later && named_at_boundary(queue, written));
    }

    if (!alive) {
      const auto& written = call.outputs.front();
      return fail(std::format("step '{}': queue element {} ('{}') writes '{}.{}', but nothing inside the queue reads "
                              "it and neither 'output' nor 'resident' names it — the pass is dead",
                              queue.name, i + 1, call.label, written.buffer_name(), written.field_name()));
    }
  }

  return result;
}

namespace {
// Докуда простирается группа слияния, начатая элементом first. Условия — в комментарии к
// `fusion_tile_bytes`; здесь важно, что группа берётся ПОДРЯД ИДУЩЕЙ и жадно: перестановка порядка
// вызовов очереди изменила бы результат, поэтому её никто и не делает.
//
// Программа devils_script в группу пока не входит, хотя структурно она такой же pointwise: её
// исполнение живёт в слое `originator_script`, а вход в него на каждой плитке создавал бы контекст
// виртуальной машины заново. Форма для этого есть (тело получает поддиапазон), но ей нужен свой
// подготовленный вызов — как `prepared_call` у нативного инструмента.
size_t fusion_group_end(const computation_queue& queue, const size_t first) noexcept {
  const auto& head = queue.calls[first];
  if (head.shape != aperture::pointwise || head.tool == nullptr) {
    return first + 1;
  }

  size_t last = first + 1;
  while (last < queue.calls.size()) {
    const auto& next = queue.calls[last];
    // Диапазоны обязаны быть РАВНЫ. У вызова со счётчиком равенство до исполнения не проверить,
    // поэтому такие сливаются только когда ссылаются на ОДНО И ТО ЖЕ поле счётчика — тогда
    // равенство выполняется по построению, а не по совпадению чисел.
    const bool same_range = head.indirect() || next.indirect()
                              ? head.indirect() && next.indirect() && head.count_from.same_field_as(next.count_from)
                              : next.range_begin == head.range_begin && next.range_end == head.range_end;
    const bool fusable = next.shape == aperture::pointwise && next.tool != nullptr && same_range;
    if (!fusable) {
      break;
    }
    ++last;
  }
  return last;
}

// Рабочий набор группы на один элемент. Поле считается ОДИН раз: промежуточное поле, которое один
// вызов пишет, а следующий читает, и есть то, что слияние оставляет в кэше, — учесть его дважды
// значило бы занизить плитку ровно там, где слияние работает.
size_t group_element_bytes(const std::span<const queue_call>& group) {
  std::vector<field_ref> counted;
  size_t total = 0;

  const auto account = [&](const field_ref& field) {
    for (const auto& known : counted) {
      if (known.same_field_as(field)) {
        return;
      }
    }
    counted.push_back(field);
    total += field.type().byte_size();
  };

  for (const auto& call : group) {
    for (const auto& binding : call.inputs) {
      account(binding);
    }
    for (const auto& binding : call.outputs) {
      account(binding);
    }
  }

  return std::max<size_t>(total, 1);
}

// Диапазон вызова В МОМЕНТ ИСПОЛНЕНИЯ. У обычного он объявлен, у вызова со счётчиком читается из
// поля — и читается именно здесь, потому что писал его предыдущий элемент этой же очереди.
struct resolved_range {
  size_t begin = 0;
  size_t end = 0;
  bool clamped = false;
};

resolved_range resolve_range(const queue_call& call) {
  if (!call.indirect()) {
    return resolved_range{call.range_begin, call.range_end, false};
  }

  resolved_range result;
  const size_t capacity = call.outputs.empty() ? 0 : call.outputs.front().count();
  result.end = read_count_field(call.count_from, capacity, result.clamped);
  return result;
}

// Слитое исполнение: один обход данных на всю группу.
bool run_group(const computation_queue& queue, const std::span<const queue_call>& group, thread::atomic_pool* pool) {
  // Диапазон у группы ОДИН: он либо объявлен одинаково, либо читается из одного и того же счётчика —
  // именно это и проверяет группировка, поэтому здесь достаточно разрешить его один раз.
  const auto range = resolve_range(group.front());
  const size_t begin = range.begin;
  const size_t end = range.end;
  if (end <= begin) {
    return range.clamped;
  }

  std::vector<prepared_call> prepared;
  prepared.reserve(group.size());
  for (const auto& call : group) {
    prepared.emplace_back(*call.tool, call.inputs, call.outputs, call.params, call.seed, begin, end, queue.name,
                          nullptr);
  }

  const size_t tile = std::max(fusion_tile_bytes / group_element_bytes(group), minimum_fusion_tile);
  const size_t tile_count = (end - begin + tile - 1) / tile;

  const auto run_tiles = [&prepared, begin, end, tile](const size_t first, const size_t last) {
    for (size_t index = first; index < last; ++index) {
      const size_t from = begin + index * tile;
      const size_t to = std::min(from + tile, end);
      // ПОРЯДОК ВНУТРИ ПЛИТКИ ЗНАЧИМ: вызовы идут по очереди, поэтому элемент i второго вызова
      // читает то, что первый вызов только что посчитал, — и читает из кэша.
      for (const auto& call : prepared) {
        call.run(from, to);
      }
    }
  };

  if (pool == nullptr || pool->size() == 0 || tile_count <= 1) {
    run_tiles(0, tile_count);
    return range.clamped;
  }

  // Плитки группируются в задачи по той же причине, что чанки свёртки: очередь пула ограничена, и
  // класть в неё десятки тысяч задач нельзя. Внутри задачи плитки идут подряд — так и надо, кэш
  // греется на своей плитке, а не на чужой.
  const size_t task_count = std::min(tile_count, (pool->size() + 1) * 4);
  const size_t tiles_per_task = (tile_count + task_count - 1) / task_count;
  for (size_t first = 0; first < tile_count; first += tiles_per_task) {
    pool->submit(run_tiles, first, std::min(first + tiles_per_task, tile_count));
  }

  pool->compute();
  pool->wait();
  return range.clamped;
}
} // namespace

queue_report run_queue(const computation_queue& queue, thread::atomic_pool* pool) {
  const auto check = check_queue(queue);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  queue_report report;
  report.calls = queue.calls.size();

  for (size_t first = 0; first < queue.calls.size();) {
    const size_t last = fusion_group_end(queue, first);

    // Группа из одного вызова идёт обычным путём. Плиточный обход ей ничего не даёт — переиспользовать
    // нечего, — а за плитки пришлось бы платить; заодно одиночный вызов остаётся ровно тем, чем был.
    if (last - first == 1) {
      const auto& call = queue.calls[first];
      const auto range = resolve_range(call);
      report.clamped += size_t(range.clamped);

      if (call.tool != nullptr) {
        dispatch(*call.tool, call.inputs, call.outputs, call.params, call.seed, range.begin, range.end, queue.name,
                 pool);
      } else if (call.indirect()) {
        // Чужое тело получает уже разрешённый диапазон: счётчик читает очередь, а не тело, — иначе
        // у каждого слоя появился бы свой способ его прочитать.
        queue_call counted = call;
        counted.range_begin = range.begin;
        counted.range_end = range.end;
        counted.body(counted, queue.name, pool);
      } else {
        call.body(call, queue.name, pool);
      }
    } else {
      const bool clamped =
        run_group(queue, std::span<const queue_call>(queue.calls).subspan(first, last - first), pool);
      report.clamped += size_t(clamped);
      report.fused += last - first;
    }

    ++report.passes;
    first = last;
  }

  return report;
}

} // namespace originator
} // namespace devils_engine
