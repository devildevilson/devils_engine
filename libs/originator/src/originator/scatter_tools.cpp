#include <algorithm>
#include <limits>
#include <numeric>
#include <vector>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

// Инструменты с апертурой scatter: они пишут по чужим индексам.
//
// Произвольного параллельного разброса в originator НЕТ и не будет: детерминизм даёт не апертура, а
// конкретный алгоритм. Поэтому scatter существует только как эти готовые инструменты, каждый со
// своей схемой фаз, а не как право, которое можно выдать любому телу.
//
// Общий приём обоих — ФИКСИРОВАННОЕ разбиение на чанки. Порядок внутри группы и порядок сложения
// определяются номером чанка и индексом элемента, а не тем, какой поток что успел, поэтому
// результат один и тот же при любом числе потоков и при исполнении в один поток.

namespace devils_engine {
namespace originator {

namespace {

// Размер чанка задан константой, а не числом потоков: от него зависит порядок результата.
constexpr size_t scatter_chunk_size = 65536;

// Верхняя граница на таблицу счётчиков (чанки x корзины). Выше неё двухфазная схема стоит дороже,
// чем экономит, и инструмент честно уходит в один поток вместо тихого разрастания памяти.
constexpr size_t maximum_counter_table = 8u << 20;

size_t chunk_count_of(const size_t count) noexcept {
  return (count + scatter_chunk_size - 1) / scatter_chunk_size;
}

// Ключ корзины из значения поля. Отрицательное и выходящее за границы — ошибка данных, а не повод
// молча положить элемент в нулевую корзину.
size_t bucket_of(const double value, const size_t bucket_count, const tool_call& call, const size_t element) {
  if (value < 0.0) {
    utils::error{}("originator step '{}': tool '{}' got a negative key {} at element {}",
                   call.step_name, call.tool_name, value, element);
  }
  const auto bucket = size_t(value);
  if (bucket >= bucket_count) {
    utils::error{}("originator step '{}': tool '{}' got key {} at element {}, but only {} buckets are declared",
                   call.step_name, call.tool_name, bucket, element, bucket_count);
  }
  return bucket;
}

// group_by: ключ -> (смещения групп, индексы элементов, отсортированные по группе).
//
// Раскладка выходов — обычный CSR: offsets[b] — начало группы b, offsets[bucket_count] —总 итог,
// то есть сколько элементов вообще разложено. Порядок внутри группы — по индексу элемента.
void tool_group_by(const tool_call& call, const size_t begin, const size_t end) {
  const auto keys = call.input(0).read();
  auto offsets = call.output(0).write();
  auto indices = call.output(1).write();

  if (offsets.count() < 2) {
    utils::error{}("originator step '{}': group_by needs an offsets buffer of at least 2 elements, got {}",
                   call.step_name, offsets.count());
  }

  const size_t bucket_count = offsets.count() - 1;
  const size_t count = end > begin ? end - begin : 0;
  const size_t chunks = chunk_count_of(count);

  if (indices.count() < count) {
    utils::error{}("originator step '{}': group_by writes {} indices, but the buffer holds {}",
                   call.step_name, count, indices.count());
  }

  const bool table_fits = chunks != 0 && bucket_count != 0 && chunks <= maximum_counter_table / bucket_count;
  const size_t effective_chunks = table_fits ? chunks : 1;
  const size_t effective_chunk_size = table_fits ? scatter_chunk_size : count;

  // Фаза 1: подсчёт. У каждого чанка своя строка таблицы, поэтому фаза не имеет ни атомиков, ни
  // порядка — счётчик от порядка не зависит.
  std::vector<size_t> counters(effective_chunks * bucket_count, 0);

  const auto count_chunk = [&](const size_t chunk) {
    const size_t first = begin + chunk * effective_chunk_size;
    const size_t last = std::min(first + effective_chunk_size, end);
    size_t* row = counters.data() + chunk * bucket_count;
    for (size_t i = first; i < last; ++i) {
      row[bucket_of(keys.get(i), bucket_count, call, i)] += 1;
    }
  };

  if (call.pool != nullptr && call.pool->size() != 0 && effective_chunks > 1) {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      call.pool->submit([&count_chunk](const size_t index) { count_chunk(index); }, chunk);
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      count_chunk(chunk);
    }
  }

  // Фаза 2: префиксная сумма по (корзина, чанк) — последовательно и в фиксированном порядке. Здесь
  // же определяется, куда каждый чанк пишет свою часть группы, поэтому порядок внутри группы
  // получается «по номеру чанка, затем по индексу элемента».
  std::vector<size_t> cursors(effective_chunks * bucket_count, 0);
  size_t running = 0;
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    offsets.set(bucket, double(running));
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      cursors[chunk * bucket_count + bucket] = running;
      running += counters[chunk * bucket_count + bucket];
    }
  }
  offsets.set(bucket_count, double(running));

  // Фаза 3: заполнение. Каждый чанк пишет в свой заранее вычисленный отрезок, поэтому записи не
  // пересекаются, а порядок не зависит от того, кто когда успел.
  const auto fill_chunk = [&](const size_t chunk) {
    const size_t first = begin + chunk * effective_chunk_size;
    const size_t last = std::min(first + effective_chunk_size, end);
    size_t* row = cursors.data() + chunk * bucket_count;
    for (size_t i = first; i < last; ++i) {
      const size_t bucket = bucket_of(keys.get(i), bucket_count, call, i);
      indices.set(row[bucket], double(i));
      row[bucket] += 1;
    }
  };

  if (call.pool != nullptr && call.pool->size() != 0 && effective_chunks > 1) {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      call.pool->submit([&fill_chunk](const size_t index) { fill_chunk(index); }, chunk);
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      fill_chunk(chunk);
    }
  }
}

// accumulate: ключ + значение -> сумма по корзинам.
//
// Каждый чанк складывает в свой приватный срез, а слияние идёт СТРОГО по номеру чанка. Поэтому
// сумма плавающих чисел воспроизводится бит в бит при любом числе потоков.
void tool_accumulate(const tool_call& call, const size_t begin, const size_t end) {
  const auto keys = call.input(0).read();
  const auto values = call.input(1).read();
  auto sums = call.output(0).write();

  const size_t bucket_count = sums.count();
  if (bucket_count == 0) {
    return;
  }

  const size_t count = end > begin ? end - begin : 0;
  const size_t chunks = chunk_count_of(count);
  const bool table_fits = chunks != 0 && chunks <= maximum_counter_table / bucket_count;
  const size_t effective_chunks = table_fits ? chunks : 1;
  const size_t effective_chunk_size = table_fits ? scatter_chunk_size : count;

  const bool reset = call.params->number("reset", 1.0) != 0.0;
  if (reset) {
    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
      sums.set(bucket, 0.0);
    }
  }

  std::vector<double> partials(effective_chunks * bucket_count, 0.0);

  const auto accumulate_chunk = [&](const size_t chunk) {
    const size_t first = begin + chunk * effective_chunk_size;
    const size_t last = std::min(first + effective_chunk_size, end);
    double* row = partials.data() + chunk * bucket_count;
    for (size_t i = first; i < last; ++i) {
      row[bucket_of(keys.get(i), bucket_count, call, i)] += values.get(i);
    }
  };

  if (call.pool != nullptr && call.pool->size() != 0 && effective_chunks > 1) {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      call.pool->submit([&accumulate_chunk](const size_t index) { accumulate_chunk(index); }, chunk);
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      accumulate_chunk(chunk);
    }
  }

  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    double total = sums.get(bucket);
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      total += partials[chunk * bucket_count + bucket];
    }
    sums.set(bucket, total);
  }
}

} // namespace

void tool_registry::add_scatter_tools() {
  add(tool_description{.name = "group_by", .shape = aperture::scatter, .input_count = 1, .output_count = 2,
                       .body = tool_group_by});
  add(tool_description{.name = "accumulate", .shape = aperture::scatter, .input_count = 2, .output_count = 1,
                       .body = tool_accumulate});
}

} // namespace originator
} // namespace devils_engine
