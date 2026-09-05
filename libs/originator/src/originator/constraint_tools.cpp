#include <algorithm>
#include <bit>
#include <memory>
#include <queue>
#include <vector>

#include "devils_engine/originator/tools.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/shared.h"

// РЕШАТЕЛЬ ОГРАНИЧЕНИЙ: то, что известно как wave function collapse.
//
// Название обманывает — квантовой механики здесь нет. Это распространение ограничений (arc
// consistency) со случайным выбором:
//
//   НАБЛЮДЕНИЕ    берётся клетка, у которой осталось меньше всего вариантов, и в ней случайно по
//                 весам выбирается ОДИН; остальные вычёркиваются;
//   РАСПРОСТРАНЕНИЕ у соседа вычёркивается всё, что больше не поддержано ни одним оставшимся
//                 вариантом клетки, и так транзитивно, пока волна не успокоится.
//
// Кончается либо тем, что у каждой клетки ровно один вариант, либо ПРОТИВОРЕЧИЕМ — клеткой, где не
// осталось ни одного. Противоречие здесь не сбой реализации, а нормальный исход: arc consistency
// только отсекает, существование решения она не гарантирует. Поэтому у вызова есть объявленное число
// попыток, а исчерпание попыток — ГРОМКИЙ отказ: тихо оставленная недособранная сетка означала бы
// другой мир под тем же зерном, и по результату этого не видно.
//
// ЗАЧЕМ ОН НУЖЕН, когда есть шум, вороной и заливка по графу: ничто из них не выражает ЖЁСТКОГО
// ЛОКАЛЬНОГО ЗАПРЕТА — «пустыня никогда не граничит с тундрой», «дверь всегда между двумя комнатами».
// Вот это он и покупает, и по этому назван — по делу, а не по алгоритму.
//
// АПЕРТУРА `sequential`, и это не ограничение реализации: какую клетку наблюдать следующей, решает
// поле, оставшееся после предыдущего распространения. Отсюда сразу и то, что в очередь он не
// попадает (её отказ для `sequential` говорит ровно это), и то, что на устройство он не поедет:
// параллельные варианты WFC существуют, но дают ДРУГОЙ результат.
//
// ДЕТЕРМИНИЗМ. Классика сравнивает шенноновскую энтропию во float, и тогда выбор клетки зависит от
// сравнения плавающих чисел. Здесь критерий ЦЕЛОЧИСЛЕННЫЙ — меньше всего оставшихся вариантов, — а
// ничья ломается хешем от номера клетки и зерна. Тот же приём, что у `graph_flood`, где порядок
// задаёт тройка `(расстояние, метка, клетка)`. Энтропия остаётся ЭВРИСТИКОЙ, а не определением
// задачи, поэтому заменить её точным критерием ничего не ломает.
//
// ПАМЯТЬ названа вслух: волна это `клетки x ceil(тайлы/32)` слов, и БОЛЬШЕ НИЧЕГО. Классическая
// реализация держит ещё счётчики поддержки на (клетка, тайл, направление) — для 512x512 при 64
// тайлах это 64 МиБ, — и здесь их нет намеренно: поддержка считается объединением битовых наборов на
// месте. Это размен времени на память, и делать его наоборот стоит только после замера.

namespace devils_engine {
namespace originator {

namespace {

// Осей ДВЕ, а направлений четыре: обратное направление ВЫВОДИТСЯ транспонированием, а не
// объявляется. Два списка одной истины однажды разъехались бы — и разъехались бы молча, потому что
// несимметричная таблица выглядит как обычная и просто чаще противоречит.
constexpr size_t primary_axes = 2;

struct collapse_rules {
  size_t tiles = 0;
  size_t words = 0;
  // forward[axis][a] — набор тайлов, которые могут стоять по +axis от a.
  std::vector<uint32_t> forward;
  // backward[axis][b] — набор тайлов a, для которых b разрешён по +axis. Транспонирование forward.
  std::vector<uint32_t> backward;
  std::vector<double> weights;
};

size_t table_index(const collapse_rules& rules, const size_t axis, const size_t tile) noexcept {
  return (axis * rules.tiles + tile) * rules.words;
}

std::shared_ptr<void> prepare_collapse(const tool_call& call) {
  const auto weights = call.input(0).read();
  const auto allowed = call.input(1).read();

  auto rules = std::make_shared<collapse_rules>();
  rules->tiles = weights.count();
  if (rules->tiles == 0) {
    utils::error{}("originator step '{}': tool '{}' got no tiles at all", call.step_name, call.tool_name);
  }
  rules->words = (rules->tiles + 31) / 32;

  // ПРАВИЛА ОБЪЯВЛЯЮТСЯ МАТРИЦЕЙ, а не битовыми словами: `allowed[(axis * tiles + a) * tiles + b]`
  // ненулевое означает «b может стоять по +axis от a». Битовый набор — внутреннее представление
  // решателя, и знать о нём конфигу незачем; иначе раскладка слов оказалась бы записана дважды — в
  // инструменте и в каждом скрипте, который таблицу заполняет.
  const size_t declared = primary_axes * rules->tiles * rules->tiles;
  if (allowed.count() < declared) {
    utils::error{}("originator step '{}': tool '{}' needs a rule matrix of {} elements for {} tiles (two axes, "
                   "tile by tile), and '{}.{}' holds {}",
                   call.step_name, call.tool_name, declared, rules->tiles,
                   call.input(1).buffer_name(), call.input(1).field_name(), allowed.count());
  }

  const size_t packed = primary_axes * rules->tiles * rules->words;
  rules->forward.assign(packed, 0);
  // ОБРАТНОЕ НАПРАВЛЕНИЕ ВЫВОДИТСЯ транспонированием, а не объявляется: два списка одной истины
  // разъехались бы молча, потому что несимметричная таблица выглядит как обычная и просто чаще
  // противоречит.
  rules->backward.assign(packed, 0);
  for (size_t axis = 0; axis < primary_axes; ++axis) {
    for (size_t a = 0; a < rules->tiles; ++a) {
      for (size_t b = 0; b < rules->tiles; ++b) {
        if (allowed.get((axis * rules->tiles + a) * rules->tiles + b) == 0.0) continue;
        rules->forward[table_index(*rules, axis, a) + b / 32] |= 1u << (b % 32);
        rules->backward[table_index(*rules, axis, b) + a / 32] |= 1u << (a % 32);
      }
    }
  }

  rules->weights.resize(rules->tiles);
  double total = 0.0;
  for (size_t i = 0; i < rules->tiles; ++i) {
    const double weight = weights.get(i);
    if (weight < 0.0) {
      utils::error{}("originator step '{}': tool '{}' got a negative weight {} for tile {}",
                     call.step_name, call.tool_name, weight, i);
    }
    rules->weights[i] = weight;
    total += weight;
  }
  if (total <= 0.0) {
    utils::error{}("originator step '{}': tool '{}' got zero total weight — every tile is impossible, and that is "
                   "a contradiction declared before the first observation",
                   call.step_name, call.tool_name);
  }

  return rules;
}

// Очередь наблюдения. Устроена как у `graph_flood`: устаревшие записи не удаляются, а отбрасываются
// при снятии — сравнением с текущим числом вариантов. Порядок задают ТОЛЬКО целые, поэтому он один и
// тот же на любой машине.
struct observation {
  uint32_t options = 0;
  uint32_t tie = 0;
  uint32_t cell = 0;

  bool operator<(const observation& other) const noexcept {
    if (options != other.options) return options > other.options;
    if (tie != other.tie) return tie > other.tie;
    return cell > other.cell;
  }
};

void tool_collapse(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': tool '{}' solves the WHOLE grid, so its range must start at 0, got [{}, {})",
                   call.step_name, call.tool_name, begin, end);
  }

  const auto& rules = *static_cast<const collapse_rules*>(call.shared);
  auto target = call.output(0).write();

  const auto shape = resolve_extent(call, call.output(0), "width", "height");
  const size_t width = shape.x;
  const size_t height = shape.y == 0 ? 1 : shape.y;
  const size_t count = end > begin ? end - begin : 0;
  if (width * height != count) {
    utils::error{}("originator step '{}': tool '{}' got a range of {} elements over a {}x{} raster",
                   call.step_name, call.tool_name, count, width, height);
  }
  if (count == 0) {
    return;
  }

  const bool wrap = call.params->number("wrap", 0.0) != 0.0;
  const auto attempts = size_t(std::max<int64_t>(call.params->integer("attempts", 16), 1));

  const size_t words = rules.words;
  const size_t tiles = rules.tiles;

  // Хвост последнего слова обязан быть чистым: лишний бит означал бы несуществующий тайл, который
  // никогда не вычеркнется и не даст клетке собраться.
  std::vector<uint32_t> everything(words, 0xffffffffu);
  if ((tiles % 32) != 0) {
    everything.back() = (1u << (tiles % 32)) - 1u;
  }

  // ЗАРАНЕЕ ЗАНЯТЫЕ КЛЕТКИ — необязательный вход. Ноль означает «клетка свободна», как и у остальных
  // инструментов с ключом; иначе это номер тайла плюс один.
  const bool has_given = call.has_input(2);
  const auto given = has_given ? call.input(2).read() : const_field_accessor{};

  std::vector<uint32_t> wave(count * words, 0);
  std::vector<uint32_t> stack;
  std::priority_queue<observation> pending;

  const auto options_of = [&](const size_t cell) {
    uint32_t total = 0;
    for (size_t w = 0; w < words; ++w) {
      total += uint32_t(std::popcount(wave[cell * words + w]));
    }
    return total;
  };

  const auto only_option = [&](const size_t cell) {
    for (size_t w = 0; w < words; ++w) {
      const auto value = wave[cell * words + w];
      if (value != 0) {
        return uint32_t(w * 32 + size_t(std::countr_zero(value)));
      }
    }
    return uint32_t(0);
  };

  // Распространение: у соседа остаётся только то, что поддержано хоть одним оставшимся вариантом
  // клетки. Поддержка считается ОБЪЕДИНЕНИЕМ строк таблицы — счётчиков поддержки здесь нет, и это
  // названный размен времени на память.
  const auto propagate = [&]() {
    std::vector<uint32_t> support(words, 0);
    while (!stack.empty()) {
      const size_t cell = stack.back();
      stack.pop_back();
      const size_t x = cell % width;
      const size_t y = cell / width;

      for (size_t axis = 0; axis < primary_axes; ++axis) {
        for (int step = -1; step <= 1; step += 2) {
          int64_t nx = int64_t(x);
          int64_t ny = int64_t(y);
          (axis == 0 ? nx : ny) += step;

          const int64_t side = axis == 0 ? int64_t(width) : int64_t(height);
          int64_t& moved = axis == 0 ? nx : ny;
          if (moved < 0 || moved >= side) {
            if (!wrap) continue;
            moved = (moved + side) % side;
          }
          const size_t neighbour = size_t(ny) * width + size_t(nx);

          std::fill(support.begin(), support.end(), 0u);
          for (size_t w = 0; w < words; ++w) {
            uint32_t bits = wave[cell * words + w];
            while (bits != 0) {
              const auto bit = size_t(std::countr_zero(bits));
              bits &= bits - 1;
              const auto* row = (step > 0 ? rules.forward.data() : rules.backward.data()) +
                                table_index(rules, axis, w * 32 + bit);
              for (size_t k = 0; k < words; ++k) {
                support[k] |= row[k];
              }
            }
          }

          bool changed = false;
          uint32_t left = 0;
          for (size_t w = 0; w < words; ++w) {
            const uint32_t before = wave[neighbour * words + w];
            const uint32_t after = before & support[w];
            changed = changed || after != before;
            wave[neighbour * words + w] = after;
            left += uint32_t(std::popcount(after));
          }

          if (left == 0) {
            return false;
          }
          if (changed) {
            pending.push(observation{left, utils::shared::prng2(uint32_t(neighbour), uint32_t(call.seed)),
                                     uint32_t(neighbour)});
            stack.push_back(uint32_t(neighbour));
          }
        }
      }
    }
    return true;
  };

  for (size_t attempt = 0; attempt < attempts; ++attempt) {
    const auto attempt_seed = utils::shared::prng2(uint32_t(call.seed), uint32_t(attempt) + 1u);
    uint32_t step_counter = 0;

    for (size_t cell = 0; cell < count; ++cell) {
      std::copy(everything.begin(), everything.end(), wave.begin() + cell * words);
    }
    stack.clear();
    pending = std::priority_queue<observation>{};

    bool alive = true;
    for (size_t cell = 0; cell < count && alive; ++cell) {
      if (has_given) {
        const auto declared = size_t(given.get(cell));
        if (declared != 0) {
          const size_t tile = declared - 1;
          if (tile >= tiles) {
            utils::error{}("originator step '{}': tool '{}' was given tile {} at element {}, but only {} tiles are "
                           "declared (0 means the cell is free, so the value is the tile plus one)",
                           call.step_name, call.tool_name, tile, cell, tiles);
          }
          std::fill(wave.begin() + cell * words, wave.begin() + (cell + 1) * words, 0u);
          wave[cell * words + tile / 32] = 1u << (tile % 32);
          stack.push_back(uint32_t(cell));
        }
      }
      pending.push(observation{uint32_t(tiles), utils::shared::prng2(uint32_t(cell), uint32_t(call.seed)),
                               uint32_t(cell)});
    }

    alive = propagate();

    while (alive) {
      // НАБЛЮДЕНИЕ. Устаревшая запись отбрасывается сравнением с текущим числом вариантов — тот же
      // приём, что у заливки по графу.
      size_t chosen = count;
      while (!pending.empty()) {
        const auto entry = pending.top();
        pending.pop();
        const auto now = options_of(entry.cell);
        if (now != entry.options || now <= 1) {
          continue;
        }
        chosen = entry.cell;
        break;
      }
      if (chosen == count) {
        break; // всё собрано
      }

      double total = 0.0;
      for (size_t w = 0; w < words; ++w) {
        uint32_t bits = wave[chosen * words + w];
        while (bits != 0) {
          const auto bit = size_t(std::countr_zero(bits));
          bits &= bits - 1;
          total += rules.weights[w * 32 + bit];
        }
      }

      // Выбор по весам идёт в ФИКСИРОВАННОМ порядке номеров тайлов: сумма плавающих чисел зависит от
      // порядка, а порядок здесь один и тот же всюду.
      const auto roll = double(utils::shared::prng_normalize(
                          utils::shared::prng2(attempt_seed, ++step_counter))) * total;
      double running = 0.0;
      size_t picked = tiles;
      for (size_t w = 0; w < words && picked == tiles; ++w) {
        uint32_t bits = wave[chosen * words + w];
        while (bits != 0) {
          const auto bit = size_t(std::countr_zero(bits));
          bits &= bits - 1;
          const size_t tile = w * 32 + bit;
          running += rules.weights[tile];
          if (running > roll || total <= 0.0) {
            picked = tile;
            break;
          }
        }
      }
      if (picked == tiles) {
        // Все веса оставшихся вариантов нулевые: выбирать не из чего, и это противоречие данных, а не
        // случайности. Берётся первый — но объявленный ноль веса означает «никогда», поэтому такой
        // набор честнее считать противоречием и перезапуститься.
        alive = false;
        break;
      }

      std::fill(wave.begin() + chosen * words, wave.begin() + (chosen + 1) * words, 0u);
      wave[chosen * words + picked / 32] = 1u << (picked % 32);
      stack.push_back(uint32_t(chosen));
      alive = propagate();
    }

    if (!alive) {
      continue;
    }

    for (size_t cell = 0; cell < count; ++cell) {
      target.set(cell, double(only_option(cell)));
    }
    if (call.has_output(1)) {
      call.output(1).write().set(0, double(attempt + 1));
    }
    return;
  }

  utils::error{}("originator step '{}': tool '{}' contradicted itself on all {} attempts over a {}x{} raster — the "
                 "rules admit no assignment reachable this way, and a half-filled grid is not an answer",
                 call.step_name, call.tool_name, attempts, width, height);
}

} // namespace

void tool_registry::add_constraint_tools() {
  add(tool_description{
    .name = "collapse", .shape = aperture::sequential,
    .input_count = 3, .optional_inputs = 1, .output_count = 2, .optional_outputs = 1,
    .body = tool_collapse, .prepare = prepare_collapse});
}

} // namespace originator
} // namespace devils_engine
