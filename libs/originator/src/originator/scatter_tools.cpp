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

// ВРЕМЕННАЯ СТОИМОСТЬ ДВУХФАЗНОЙ СХЕМЫ: таблица «чанки x корзины», которой нет ни в одном объявлении
// буферов. Считается по тому же правилу, по которому она и заводится, — включая потолок, после
// которого инструмент честно уходит в один чанк вместо тихого разрастания памяти.
//
// `bytes_per_cell` у каждого инструмента свой: `group_by` держит счётчики и курсоры, `accumulate` —
// плавающие частичные суммы, `count_by` — целые.
template <size_t bytes_per_cell>
size_t counter_table_footprint(const tool_call& call) {
  if (call.outputs.empty() || !call.outputs.front().valid()) {
    return 0;
  }

  const size_t bucket_count = call.outputs.front().count() > 1 ? call.outputs.front().count() - 1 : 1;
  const size_t count = call.range_count();
  const size_t chunks = chunk_count_of(count);
  const bool table_fits = chunks != 0 && bucket_count != 0 && chunks <= maximum_counter_table / bucket_count;
  const size_t effective_chunks = table_fits ? chunks : 1;
  return effective_chunks * bucket_count * bytes_per_cell;
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

// count_by: ключ -> СКОЛЬКО элементов попало в корзину.
//
// От `accumulate` отличается двумя вещами, и обе принципиальны.
//
// СЧИТАЕТ ЦЕЛЫМИ. Целое сложение коммутативно, поэтому у результата нет зависимости от порядка — ни
// от порядка чанков, ни от порядка прихода групп на устройстве. Именно это и позволяет инструменту
// объявить `order_free_writes` и попасть в очередь: `scatter` отклоняется за ПОРЯДОК записей, а
// здесь порядка нет вовсе (§4.3 бьёт по плавающей свёртке, а не по свёртке как таковой).
//
// НЕ ОБНУЛЯЕТ приёмник. `accumulate` по умолчанию обнуляет, а здесь этого нельзя: на устройстве
// атомик умеет только прибавлять, и обнуление внутри того же прохода означало бы гонку. Одно
// объявление с двумя поведениями хуже, чем лишний вызов, поэтому обнуление — отдельный `fill`, и
// очередь считает его ЖИВЫМ ровно потому, что накопитель читает то, во что пишет.
void tool_count_by(const tool_call& call, const size_t begin, const size_t end) {
  const auto keys = call.input(0).read();
  auto counts = call.output(0).write();

  const size_t bucket_count = counts.count();
  if (bucket_count == 0) {
    return;
  }

  const size_t count = end > begin ? end - begin : 0;
  const size_t chunks = chunk_count_of(count);
  const bool table_fits = chunks != 0 && chunks <= maximum_counter_table / bucket_count;
  const size_t effective_chunks = table_fits ? chunks : 1;
  const size_t effective_chunk_size = table_fits ? scatter_chunk_size : count;

  std::vector<uint64_t> partials(effective_chunks * bucket_count, 0);

  const auto count_chunk = [&](const size_t chunk) {
    const size_t first = begin + chunk * effective_chunk_size;
    const size_t last = std::min(first + effective_chunk_size, end);
    uint64_t* row = partials.data() + chunk * bucket_count;
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

  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    uint64_t total = uint64_t(counts.get(bucket));
    for (size_t chunk = 0; chunk < effective_chunks; ++chunk) {
      total += partials[chunk * bucket_count + bucket];
    }
    counts.set(bucket, double(total));
  }
}

} // namespace

void tool_registry::add_scatter_tools() {
  add(tool_description{.name = "group_by", .shape = aperture::scatter, .input_count = 1, .output_count = 2,
                       .body = tool_group_by, .footprint = counter_table_footprint<sizeof(size_t) * 2>});
  add(tool_description{.name = "accumulate", .shape = aperture::scatter, .input_count = 2, .output_count = 1,
                       .body = tool_accumulate, .footprint = counter_table_footprint<sizeof(double)>});
  add(tool_description{
    .name = "count_by", .shape = aperture::scatter, .input_count = 1, .output_count = 1,
    .body = tool_count_by, .footprint = counter_table_footprint<sizeof(uint64_t)>,
    // КЛЮЧ ВНЕ ДИАПАЗОНА: на хосте это громкая ошибка данных, на устройстве бросить нечем, и там
    // такой элемент пропускается. Расхождение бывает только у конфига, который на CPU уже падает, —
    // поэтому оно названо здесь, а не спрятано.
    // УЗКОЕ МЕСТО СВЁРТКИ НА УСТРОЙСТВЕ — НЕ ДВУХСТАДИЙНОСТЬ, А КОНКУРЕНЦИЯ. Замер GN04 показал
    // неожиданное: гистограмма стоила ДОРОЖЕ самой разметки (`7.43` против `4.65` мс), хотя работы в
    // ней несравнимо меньше — один `atomicAdd` на пиксель против цикла по 64 сайтам. Причина в том,
    // что четверть миллиона пикселей бьётся за 64 счётчика: атомики сериализуются на одной кэш-линии.
    //
    // Форма ответа известна давно и теперь сделана: группа копит в РАЗДЕЛЯЕМОЙ памяти, а в общий
    // буфер уходит ОДИН атомик на корзину на группу. Число атомиков падает с «на элемент» до
    // «на группу», то есть в размер группы раз.
    //
    // ПУТЕЙ ДВА, и выбор между ними УНИФОРМЕН по группе (зависит только от длины буфера, а не от
    // данных): широкая гистограмма в разделяемую память не влезает, и там остаётся прежний прямой
    // атомик. Барьер внутри такого ветвления законен именно потому, что условие одинаково у всей
    // группы.
    .device_body = "  uint bins = out_0_length();\n"
                   "  if (bins <= 256u) {\n"
                   "    for (uint i = gl_LocalInvocationID.x; i < bins; i += gl_WorkGroupSize.x) {\n"
                   "      count_by_bins[i] = 0u;\n"
                   "    }\n"
                   "    barrier();\n"
                   "    if (in_range) {\n"
                   "      uint bucket = uint(in_0_at(index));\n"
                   "      if (bucket < bins) atomicAdd(count_by_bins[bucket], 1u);\n"
                   "    }\n"
                   "    barrier();\n"
                   "    for (uint i = gl_LocalInvocationID.x; i < bins; i += gl_WorkGroupSize.x) {\n"
                   "      uint total = count_by_bins[i];\n"
                   "      if (total != 0u) out_0_add(i, total);\n"
                   "    }\n"
                   "  } else if (in_range) {\n"
                   "    uint bucket = uint(in_0_at(index));\n"
                   "    if (bucket < bins) out_0_add(bucket, 1u);\n"
                   "  }\n",
    .device_prelude = "shared uint count_by_bins[256];\n",
    .device_params = {},
    // Приёмник — счётчик, то есть целое поле по природе.
    .device_integer_ready = true,
    // Барьеры требуют ВСЕЙ группы: инвокация вне диапазона обязана дойти до них и ничего не записать.
    .device_whole_group = true,
    // ТО САМОЕ ОБЪЯВЛЕНИЕ, которое пускает scatter в очередь.
    .order_free_writes = true});
}

} // namespace originator
} // namespace devils_engine
