#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <gtl/phmap.hpp>

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
// ЗДЕСЬ ТРИ ИНСТРУМЕНТА, И ЯДРО У НИХ ОДНО:
//
//   collapse       раскладка по РАСТРУ: соседство задаётся формой буфера, осей две;
//   graph_collapse раскладка по ГРАФУ соседства (CSR): у дуги нет направления, поэтому правила
//                  обязаны быть симметричны, и несимметричная таблица отклоняется вслух;
//   learn_rules    ПРАВИЛА ИЗ ОБРАЗЦА: окно NxN пробегает нарисованный образец, разные окна
//                  становятся алфавитом, а встреченные соседства — таблицей.
//
// Волна, наблюдение, распространение и откат написаны ОДИН раз: соседство приходит в ядро функцией,
// потому что различаются растр и граф ровно им. Второй экземпляр этой логики однажды разъехался бы
// с первым, и разъехался бы молча — обе версии продолжали бы выдавать «какую-то» раскладку.
//
// АПЕРТУРА РЕШАТЕЛЕЙ `sequential`, и это не ограничение реализации: какую клетку наблюдать
// следующей, решает поле, оставшееся после предыдущего распространения. Отсюда сразу и то, что в
// очередь они не попадают (её отказ для `sequential` говорит ровно это), и то, что на устройство они
// не поедут: параллельные варианты WFC существуют, но дают ДРУГОЙ результат.
//
// ДЕТЕРМИНИЗМ. Классика сравнивает шенноновскую энтропию во float, и тогда выбор клетки зависит от
// сравнения плавающих чисел. Здесь критерий ЦЕЛОЧИСЛЕННЫЙ — меньше всего оставшихся вариантов, — а
// ничья ломается хешем от номера клетки и зерна. Тот же приём, что у `graph_flood`, где порядок
// задаёт тройка `(расстояние, метка, клетка)`. Энтропия остаётся ЭВРИСТИКОЙ, а не определением
// задачи, поэтому заменить её точным критерием ничего не ломает.
//
// ПАМЯТЬ названа вслух: волна это `клетки x ceil(тайлы/32)` слов, и БОЛЬШЕ НИЧЕГО, пока не включены
// откаты. Классическая реализация держит ещё счётчики поддержки на (клетка, тайл, направление) — для
// 512x512 при 64 тайлах это 64 МиБ, — и здесь их нет намеренно: поддержка считается объединением
// битовых наборов на месте. Это размен времени на память, и делать его наоборот стоит только после
// замера. Цена откатов названа там, где они объявляются.

namespace devils_engine {
namespace originator {

namespace {

// Осей ДВЕ, а направлений четыре: обратное направление ВЫВОДИТСЯ транспонированием, а не
// объявляется. Два списка одной истины однажды разъехались бы — и разъехались бы молча, потому что
// несимметричная таблица выглядит как обычная и просто чаще противоречит.
constexpr size_t primary_axes = 2;
constexpr size_t raster_directions = primary_axes * 2;

// Номер направления растра: ось, а внутри неё «вперёд» и «назад».
constexpr size_t raster_direction(const size_t axis, const bool forward) noexcept {
  return axis * 2 + (forward ? 0 : 1);
}

// ТАБЛИЦЫ ПОДДЕРЖКИ, по одной на направление. `support_row(direction, a)` — набор тайлов, которые
// разрешено поставить у соседа в этом направлении, если здесь стоит `a`. У растра направлений
// четыре, у графа одно: дуга без направления означает симметричные правила.
struct collapse_rules {
  // РАЗМЕР ОБЪЯВЛЕННОГО алфавита: с этим шагом лежит матрица правил, потому что её писали, зная
  // объявленную ёмкость, а не найденную длину.
  size_t stride = 0;
  // РАЗМЕР ДЕЙСТВУЮЩЕГО алфавита: хвост, который ничего не весит и ни с чем не соседствует, в алфавит
  // не входит. Это не оптимизация «на глазок»: такой тайл не может быть ни выбран (нулевой вес
  // исключён из волны), ни поставлен условием (условие проверяется по правилам, а их у него нет), —
  // значит он не влияет на результат, а платить за него шириной волны пришлось бы на каждой клетке.
  // Объявленная с запасом ёмкость обучения от этого перестаёт стоить времени, оставаясь стоить
  // памяти буфера — той, которую автор и объявил.
  size_t tiles = 0;
  size_t words = 0;
  size_t directions = 0;
  std::vector<uint32_t> support;
  std::vector<double> weights;
  // НАЧАЛЬНАЯ ВОЛНА. Не «все тайлы», а все тайлы С ПОЛОЖИТЕЛЬНЫМ ВЕСОМ: объявленный ноль веса
  // означает «никогда не выбирать», и оставлять такой тайл в волне значило бы давать клетке вариант,
  // который она не может взять, — а число вариантов у решателя это КРИТЕРИЙ выбора, и врать ему
  // нельзя. Заранее занятая клетка это не выбор, а условие, поэтому её тайл ставится независимо от
  // веса: так «глубокая вода только там, где я её нарисовал» выражается весом 0.
  std::vector<uint32_t> possible;
  uint32_t possible_count = 0;
};

const uint32_t* support_row(const collapse_rules& rules, const size_t direction, const size_t tile) noexcept {
  return rules.support.data() + (direction * rules.tiles + tile) * rules.words;
}

void set_support(collapse_rules& rules, const size_t direction, const size_t tile, const size_t allowed) noexcept {
  rules.support[(direction * rules.tiles + tile) * rules.words + allowed / 32] |= 1u << (allowed % 32);
}

// Веса читаются и проверяются одинаково для растра и графа: отрицательный вес это ошибка данных, а
// нулевая сумма — противоречие, объявленное ещё до первого наблюдения. Действующая длина алфавита
// здесь ещё не известна — её досказывает матрица правил.
size_t read_weights(collapse_rules& rules, const tool_call& call, const field_ref& binding) {
  const auto weights = binding.read();
  rules.stride = weights.count();
  if (rules.stride == 0) {
    utils::error{}("originator step '{}': tool '{}' got no tiles at all", call.step_name, call.tool_name);
  }

  rules.weights.resize(rules.stride);
  size_t weighted = 0;
  double total = 0.0;
  for (size_t i = 0; i < rules.stride; ++i) {
    const double weight = weights.get(i);
    if (weight < 0.0) {
      utils::error{}("originator step '{}': tool '{}' got a negative weight {} for tile {}",
                     call.step_name, call.tool_name, weight, i);
    }
    rules.weights[i] = weight;
    total += weight;
    if (weight > 0.0) {
      weighted = i + 1;
    }
  }
  if (total <= 0.0) {
    utils::error{}("originator step '{}': tool '{}' got zero total weight — every tile is impossible, and that is "
                   "a contradiction declared before the first observation",
                   call.step_name, call.tool_name);
  }
  return weighted;
}

// НАЧАЛЬНАЯ ВОЛНА строится последней: до неё надо знать действующую длину алфавита, а её досказывает
// матрица правил.
void seal_alphabet(collapse_rules& rules, const size_t tiles) {
  rules.tiles = tiles;
  rules.words = (tiles + 31) / 32;
  rules.possible.assign(rules.words, 0);
  rules.possible_count = 0;
  for (size_t i = 0; i < tiles; ++i) {
    if (rules.weights[i] > 0.0) {
      rules.possible[i / 32] |= 1u << (i % 32);
      rules.possible_count += 1;
    }
  }
}

// ПРАВИЛА РАСТРА ОБЪЯВЛЯЮТСЯ МАТРИЦЕЙ: `allowed[(axis * tiles + a) * tiles + b]` ненулевое означает
// «b может стоять по +axis от a». Битовый набор — внутреннее представление решателя, и знать о нём
// конфигу незачем; иначе раскладка слов оказалась бы записана дважды — в инструменте и в каждом
// скрипте, который таблицу заполняет.
std::shared_ptr<void> prepare_raster_rules(const tool_call& call) {
  auto rules = std::make_shared<collapse_rules>();
  size_t tiles = read_weights(*rules, call, call.input(0));

  const auto allowed = call.input(1).read();
  const size_t stride = rules->stride;
  const size_t declared = primary_axes * stride * stride;
  if (allowed.count() < declared) {
    utils::error{}("originator step '{}': tool '{}' needs a rule matrix of {} elements for {} tiles (two axes, "
                   "tile by tile), and '{}.{}' holds {}",
                   call.step_name, call.tool_name, declared, stride,
                   call.input(1).buffer_name(), call.input(1).field_name(), allowed.count());
  }

  // Тайл, у которого есть хоть одно правило, в алфавите есть — даже с нулевым весом: вес говорит,
  // выбирать ли его, а правила говорят, существует ли он.
  for (size_t axis = 0; axis < primary_axes; ++axis) {
    for (size_t a = 0; a < stride; ++a) {
      for (size_t b = 0; b < stride; ++b) {
        if (allowed.get((axis * stride + a) * stride + b) == 0.0) continue;
        tiles = std::max({tiles, a + 1, b + 1});
      }
    }
  }
  seal_alphabet(*rules, tiles);

  rules->directions = raster_directions;
  rules->support.assign(rules->directions * tiles * rules->words, 0);
  for (size_t axis = 0; axis < primary_axes; ++axis) {
    for (size_t a = 0; a < tiles; ++a) {
      for (size_t b = 0; b < tiles; ++b) {
        if (allowed.get((axis * stride + a) * stride + b) == 0.0) continue;
        set_support(*rules, raster_direction(axis, true), a, b);
        // ОБРАТНОЕ НАПРАВЛЕНИЕ ВЫВОДИТСЯ транспонированием, а не объявляется.
        set_support(*rules, raster_direction(axis, false), b, a);
      }
    }
  }
  return rules;
}

// ПРАВИЛА ГРАФА — ОДНА матрица `tiles x tiles`, и она ОБЯЗАНА быть симметричной.
//
// У дуги графа соседства нет канонического направления: на сфере «правее» не определено вовсе, а
// CSR симметризован по построению. Значит несимметричная таблица здесь означает, что автор считал
// направление значащим, — и это не мелкая небрежность, а непонимание задачи: тихая симметризация
// (по «или» или по «и») дала бы правила, которых автор не писал. Поэтому отказ, и с называнием пары.
std::shared_ptr<void> prepare_graph_rules(const tool_call& call) {
  auto rules = std::make_shared<collapse_rules>();
  size_t tiles = read_weights(*rules, call, call.input(2));

  const auto allowed = call.input(3).read();
  const size_t stride = rules->stride;
  const size_t declared = stride * stride;
  if (allowed.count() < declared) {
    utils::error{}("originator step '{}': tool '{}' needs a rule matrix of {} elements for {} tiles (tile by tile, "
                   "and ONE matrix because a graph arc has no direction), and '{}.{}' holds {}",
                   call.step_name, call.tool_name, declared, stride,
                   call.input(3).buffer_name(), call.input(3).field_name(), allowed.count());
  }

  for (size_t a = 0; a < stride; ++a) {
    for (size_t b = 0; b < stride; ++b) {
      if (allowed.get(a * stride + b) != 0.0) {
        tiles = std::max({tiles, a + 1, b + 1});
      }
    }
  }
  seal_alphabet(*rules, tiles);

  rules->directions = 1;
  rules->support.assign(tiles * rules->words, 0);
  for (size_t a = 0; a < tiles; ++a) {
    for (size_t b = 0; b < tiles; ++b) {
      const bool forward = allowed.get(a * stride + b) != 0.0;
      const bool backward = allowed.get(b * stride + a) != 0.0;
      if (forward != backward) {
        utils::error{}("originator step '{}': tool '{}' got an asymmetric rule for the pair ({}, {}) — a graph arc "
                       "has no direction, so '{} next to {}' and '{} next to {}' are the SAME statement. Silently "
                       "taking one of them would generate rules nobody wrote",
                       call.step_name, call.tool_name, a, b, a, b, b, a);
      }
      if (forward) {
        set_support(*rules, 0, a, b);
      }
    }
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

// ЗАРАНЕЕ ЗАНЯТЫЕ КЛЕТКИ — необязательный вход. Ноль означает «клетка свободна», как и у остальных
// инструментов с ключом; иначе это номер тайла плюс один.
struct given_cells {
  const_field_accessor field;
  bool present = false;

  uint32_t at(const size_t cell) const noexcept { return present ? uint32_t(field.get(cell)) : 0u; }
};

// ОТКАТЫ: сколько раз разрешено вернуться и на сколько наблюдений назад помним.
//
// Бюджет и глубина — РАЗНЫЕ величины, и одной не хватает. Бюджет говорит, сколько работы автор готов
// потратить на спасение попытки; глубина говорит, сколько ПАМЯТИ он готов на это отдать. Откатиться
// сто раз, помня один шаг, — обычное дело; помнить сто шагов на миллионе клеток — совсем другой
// разговор, и он должен вестись явно.
struct rollback_limits {
  size_t budget = 0;
  size_t depth = 0;
};

struct solve_report {
  bool solved = false;
  size_t attempts = 0;
  size_t rollbacks = 0;
};

// ЯДРО. Одно на растр и на граф: соседство приходит функцией `neighbours(cell, fn)`, которая зовёт
// `fn(сосед, направление)`.
template <typename neighbours_t>
class collapse_solver {
public:
  collapse_solver(const tool_call& call, const collapse_rules& rules, const size_t count,
                  neighbours_t neighbours, const rollback_limits& limits) :
    call_(call), rules_(rules), words_(rules.words), count_(count), neighbours_(std::move(neighbours)),
    limits_(limits), wave_(count * rules.words, 0), support_(rules.words, 0) {
    journal_ready_ = limits_.budget > 0 && limits_.depth > 0;
    if (journal_ready_ && count_ * words_ > size_t(UINT32_MAX)) {
      utils::error{}("originator step '{}': tool '{}' cannot journal a wave of {} words for rollbacks — the undo "
                     "log addresses words with 32 bits",
                     call_.step_name, call_.tool_name, count_ * words_);
    }
  }

  solve_report run(const given_cells& given, const size_t attempts) {
    solve_report report;
    for (size_t attempt = 0; attempt < attempts; ++attempt) {
      report.attempts = attempt + 1;
      attempt_seed_ = utils::shared::prng2(uint32_t(call_.seed), uint32_t(attempt) + 1u);
      step_counter_ = 0;
      rollbacks_left_ = limits_.budget;
      reset(given);

      bool alive = propagate();
      if (!alive) alive = recover();

      while (alive) {
        const size_t chosen = observe();
        if (chosen == count_) {
          report.solved = true;
          break;
        }
        const size_t picked = pick(chosen);
        open_level(chosen, picked);
        assign(chosen, picked);
        stack_.assign(1, uint32_t(chosen));
        alive = propagate();
        if (!alive) alive = recover();
      }

      report.rollbacks = rollbacks_used_;
      if (report.solved) {
        return report;
      }
    }
    return report;
  }

  uint32_t tile_at(const size_t cell) const noexcept {
    for (size_t w = 0; w < words_; ++w) {
      const auto value = wave_[cell * words_ + w];
      if (value != 0) {
        return uint32_t(w * 32 + size_t(std::countr_zero(value)));
      }
    }
    return 0;
  }

private:
  struct change {
    uint32_t word = 0;
    uint32_t previous = 0;
  };

  struct level {
    size_t journal_start = 0;
    uint32_t cell = 0;
    uint32_t tile = 0;
  };

  // Единственная точка записи в волну. Через неё же идёт журнал отката — иначе однажды появилась бы
  // запись мимо журнала, и откат восстановил бы состояние, которого не было.
  void write_word(const size_t index, const uint32_t value) {
    if (wave_[index] == value) return;
    if (journal_ready_) {
      journal_.push_back(change{uint32_t(index), wave_[index]});
    }
    wave_[index] = value;
  }

  uint32_t options_of(const size_t cell) const noexcept {
    uint32_t total = 0;
    for (size_t w = 0; w < words_; ++w) {
      total += uint32_t(std::popcount(wave_[cell * words_ + w]));
    }
    return total;
  }

  uint32_t tie_of(const size_t cell) const noexcept {
    return utils::shared::prng2(uint32_t(cell), uint32_t(call_.seed));
  }

  void push_observation(const size_t cell, const uint32_t options) {
    pending_.push(observation{options, tie_of(cell), uint32_t(cell)});
  }

  void reset(const given_cells& given) {
    for (size_t cell = 0; cell < count_; ++cell) {
      std::copy(rules_.possible.begin(), rules_.possible.end(), wave_.begin() + cell * words_);
    }
    journal_.clear();
    levels_.clear();
    stack_.clear();

    std::vector<observation> initial;
    initial.reserve(count_);
    for (size_t cell = 0; cell < count_; ++cell) {
      const auto declared = given.at(cell);
      if (declared != 0) {
        const size_t tile = size_t(declared) - 1;
        std::fill(wave_.begin() + cell * words_, wave_.begin() + (cell + 1) * words_, 0u);
        wave_[cell * words_ + tile / 32] = 1u << (tile % 32);
        stack_.push_back(uint32_t(cell));
        continue; // заранее занятая клетка уже собрана, наблюдать её незачем
      }
      initial.push_back(observation{rules_.possible_count, tie_of(cell), uint32_t(cell)});
    }
    pending_ = std::priority_queue<observation>(std::less<observation>{}, std::move(initial));
  }

  // Распространение: у соседа остаётся только то, что поддержано хоть одним оставшимся вариантом
  // клетки. Поддержка считается ОБЪЕДИНЕНИЕМ строк таблицы — счётчиков поддержки здесь нет, и это
  // названный размен времени на память.
  bool propagate() {
    while (!stack_.empty()) {
      const size_t cell = stack_.back();
      stack_.pop_back();
      bool dead = false;

      neighbours_(cell, [&](const size_t other, const size_t direction) {
        if (dead) return;

        std::fill(support_.begin(), support_.end(), 0u);
        for (size_t w = 0; w < words_; ++w) {
          uint32_t bits = wave_[cell * words_ + w];
          while (bits != 0) {
            const auto bit = size_t(std::countr_zero(bits));
            bits &= bits - 1;
            const auto* row = support_row(rules_, direction, w * 32 + bit);
            for (size_t k = 0; k < words_; ++k) {
              support_[k] |= row[k];
            }
          }
        }

        bool changed = false;
        uint32_t left = 0;
        for (size_t w = 0; w < words_; ++w) {
          const uint32_t before = wave_[other * words_ + w];
          const uint32_t after = before & support_[w];
          if (after != before) {
            changed = true;
            write_word(other * words_ + w, after);
          }
          left += uint32_t(std::popcount(after));
        }

        if (left == 0) {
          dead = true;
          return;
        }
        if (changed) {
          push_observation(other, left);
          stack_.push_back(uint32_t(other));
        }
      });

      if (dead) {
        return false;
      }
    }
    return true;
  }

  // НАБЛЮДЕНИЕ. Устаревшая запись отбрасывается сравнением с текущим числом вариантов — тот же
  // приём, что у заливки по графу.
  size_t observe() {
    while (!pending_.empty()) {
      const auto entry = pending_.top();
      pending_.pop();
      const auto now = options_of(entry.cell);
      if (now != entry.options || now <= 1) {
        continue;
      }
      return entry.cell;
    }
    return count_; // всё собрано
  }

  // Выбор по весам идёт в ФИКСИРОВАННОМ порядке номеров тайлов: сумма плавающих чисел зависит от
  // порядка, а порядок здесь один и тот же всюду.
  size_t pick(const size_t cell) {
    double total = 0.0;
    for (size_t w = 0; w < words_; ++w) {
      uint32_t bits = wave_[cell * words_ + w];
      while (bits != 0) {
        const auto bit = size_t(std::countr_zero(bits));
        bits &= bits - 1;
        total += rules_.weights[w * 32 + bit];
      }
    }

    const auto roll = double(utils::shared::prng_normalize(
                        utils::shared::prng2(attempt_seed_, ++step_counter_))) * total;
    double running = 0.0;
    // Нулевой вес сюда не доходит: он исключён из начальной волны, а наблюдается только клетка, у
    // которой вариантов больше одного, — значит все они с положительным весом и сумма положительна.
    // Последний вариант остаётся ответом на границе `roll == total`, которой плавающая арифметика
    // достигать не обязана, но может.
    size_t last = rules_.tiles;
    for (size_t w = 0; w < words_; ++w) {
      uint32_t bits = wave_[cell * words_ + w];
      while (bits != 0) {
        const auto bit = size_t(std::countr_zero(bits));
        bits &= bits - 1;
        last = w * 32 + bit;
        running += rules_.weights[last];
        if (running > roll) {
          return last;
        }
      }
    }
    return last;
  }

  void assign(const size_t cell, const size_t tile) {
    for (size_t w = 0; w < words_; ++w) {
      write_word(cell * words_ + w, w == tile / 32 ? 1u << (tile % 32) : 0u);
    }
  }

  void open_level(const size_t cell, const size_t tile) {
    if (!journal_ready_) return;
    levels_.push_back(level{journal_.size(), uint32_t(cell), uint32_t(tile)});
    trim_history();
  }

  // Глубина истории — ОБЪЯВЛЕННАЯ величина, поэтому вытеснение старых уровней делается сразу, а не
  // «когда память кончится». Вытесненные записи журнала уже применены к волне, поэтому их удаление
  // ничего не меняет: они лишь перестают быть отменяемыми.
  void trim_history() {
    if (levels_.size() <= limits_.depth) return;
    levels_.erase(levels_.begin(), levels_.begin() + (levels_.size() - limits_.depth));

    // Сдвиг делается не на каждом наблюдении, а когда мёртвая часть перевесила живую: иначе перенос
    // байтов стоил бы дороже самого распространения.
    const size_t dead = levels_.front().journal_start;
    const size_t live = journal_.size() - dead;
    if (dead <= live + 1024) return;
    journal_.erase(journal_.begin(), journal_.begin() + dead);
    for (auto& item : levels_) {
      item.journal_start -= dead;
    }
  }

  // ОТКАТ. Противоречие означает, что где-то ВЫШЕ был сделан выбор, который сюда привёл; перезапуск
  // выбрасывает вместе с ним и всю остальную собранную сетку. Откат отменяет ровно последний выбор и
  // ЗАПРЕЩАЕТ его — без запрета решатель вернулся бы в ту же точку, и поиск не сузился бы никогда.
  //
  // Запрет записывается в журнал ОБЪЕМЛЮЩЕГО уровня и отменяется вместе с ним: он верен только в том
  // состоянии, в котором был выведен, а глубже это состояние уже не то.
  bool recover() {
    while (rollbacks_left_ > 0 && !levels_.empty()) {
      rollbacks_left_ -= 1;
      rollbacks_used_ += 1;

      const auto undone = levels_.back();
      levels_.pop_back();

      touched_.clear();
      while (journal_.size() > undone.journal_start) {
        const auto entry = journal_.back();
        journal_.pop_back();
        wave_[entry.word] = entry.previous;
        touched_.push_back(uint32_t(size_t(entry.word) / words_));
      }

      const size_t index = size_t(undone.cell) * words_ + size_t(undone.tile) / 32;
      write_word(index, wave_[index] & ~(1u << (size_t(undone.tile) % 32)));
      touched_.push_back(undone.cell);

      // Устаревшие записи очереди отбрасываются сравнением с текущим числом вариантов, поэтому
      // вернуть в неё надо ровно те клетки, у которых это число ИЗМЕНИЛОСЬ, — а их назвал сам
      // журнал. Полный пересбор очереди дал бы то же самое, но заплатил бы за это обходом всего
      // растра на каждый откат.
      for (const auto cell : touched_) {
        const auto options = options_of(cell);
        if (options > 1) {
          push_observation(cell, options);
        }
      }

      if (options_of(undone.cell) == 0) {
        continue; // запрет опустошил саму клетку: этот уровень мёртв, надо ещё назад
      }

      stack_.assign(1, undone.cell);
      if (propagate()) {
        return true;
      }
    }
    return false;
  }

  const tool_call& call_;
  const collapse_rules& rules_;
  size_t words_ = 0;
  size_t count_ = 0;
  neighbours_t neighbours_;
  rollback_limits limits_;

  std::vector<uint32_t> wave_;
  std::vector<uint32_t> support_;
  std::vector<uint32_t> stack_;
  std::vector<uint32_t> touched_;
  std::priority_queue<observation> pending_;

  bool journal_ready_ = false;
  std::vector<change> journal_;
  std::vector<level> levels_;

  uint32_t attempt_seed_ = 0;
  uint32_t step_counter_ = 0;
  size_t rollbacks_left_ = 0;
  size_t rollbacks_used_ = 0;
};

// ВРЕМЕННАЯ СТОИМОСТЬ РЕШАТЕЛЯ. Память здесь названа вслух с самого начала («волна это клетки x
// ceil(тайлы/32) слов, и БОЛЬШЕ НИЧЕГО»), но названа была в комментарии — а теперь она объявлена,
// то есть попадает в отчёт наравне с буферами.
//
// Журнал откатов считается ВЕРХНЕЙ оценкой: сколько изменений случится, до прогона не знает никто, и
// ограничен он терпением (`rollbacks`) и памятью (`history`). Занижать нельзя, поэтому берётся
// произведение, а не «обычно меньше».
size_t collapse_footprint(const tool_call& call) {
  const size_t cells = call.range_count();
  const auto tiles = size_t(std::max<int64_t>(call.params->integer("tiles", 1), 1));
  const size_t words = (tiles + 31) / 32;

  const size_t wave = cells * words * sizeof(uint32_t);
  // Поддержка, стек и отметки — по слову на клетку каждая.
  const size_t working = cells * sizeof(uint32_t) * 3;
  const auto history = size_t(std::max<int64_t>(call.params->integer("history", 0), 0));
  const auto rollbacks = size_t(std::max<int64_t>(call.params->integer("rollbacks", 0), 0));
  const size_t journal = history == 0 || rollbacks == 0 ? 0 : history * cells * sizeof(uint32_t) * 2;
  return wave + working + journal;
}

// Соседство РАСТРА: четыре направления, порядок обхода фиксирован.
struct raster_neighbours {
  size_t width = 0;
  size_t height = 0;
  bool wrap = false;

  template <typename fn_t>
  void operator()(const size_t cell, fn_t&& fn) const {
    const size_t x = cell % width;
    const size_t y = cell / width;
    for (size_t axis = 0; axis < primary_axes; ++axis) {
      const int64_t side = axis == 0 ? int64_t(width) : int64_t(height);
      for (int step = -1; step <= 1; step += 2) {
        int64_t nx = int64_t(x);
        int64_t ny = int64_t(y);
        int64_t& moved = axis == 0 ? nx : ny;
        moved += step;
        if (moved < 0 || moved >= side) {
          if (!wrap) continue;
          moved = (moved + side) % side;
        }
        fn(size_t(ny) * width + size_t(nx), raster_direction(axis, step > 0));
      }
    }
  }
};

// Соседство ГРАФА: CSR, направление одно. Дуга, ведущая за пределы диапазона, пропускается — это то
// же правило, по которому живёт `graph_flood`.
struct graph_neighbours {
  const_field_accessor offsets;
  const_field_accessor arcs;
  size_t count = 0;

  template <typename fn_t>
  void operator()(const size_t cell, fn_t&& fn) const {
    const auto first = size_t(offsets.get(cell));
    // CSR приходит ДАННЫМИ, и данные могут быть неполны: смещение за концом буфера дуг означало бы
    // чтение за концом, а не «много соседей».
    const auto last = std::min(size_t(offsets.get(cell + 1)), arcs.count());
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other >= count) continue;
      fn(other, size_t(0));
    }
  }
};

rollback_limits read_rollback_limits(const tool_call& call) {
  rollback_limits limits;
  limits.budget = size_t(std::max<int64_t>(call.params->integer("rollbacks", 0), 0));
  // Глубина по умолчанию выведена из бюджета: без откатов история не нужна вовсе и не стоит ни
  // байта, а с откатами восьми шагов хватает почти всегда — противоречие у arc consistency почти
  // всегда локально. Автор, которому мало, называет глубину сам и вместе с ней называет память.
  const int64_t fallback = limits.budget == 0 ? 0 : int64_t(std::min<size_t>(limits.budget, 8));
  limits.depth = size_t(std::max<int64_t>(call.params->integer("history", fallback), 0));
  if (limits.budget == 0) {
    limits.depth = 0;
  } else if (limits.depth == 0) {
    // Терпение без памяти — противоречивое объявление: откатываться некуда, а автор объявил, что
    // откатываться можно. Тихо превратить это в перезапуск значило бы исполнять не то, что написано.
    utils::error{}("originator step '{}': tool '{}' was allowed {} rollbacks with a history of 0 — there is "
                   "nowhere to roll back to. Declare a history, or drop the rollbacks",
                   call.step_name, call.tool_name, limits.budget);
  }
  return limits;
}

// Заранее занятые клетки проверяются ОДИН раз, до попыток: значение вне таблицы это ошибка данных, и
// повторять её на каждой попытке незачем.
given_cells read_given(const tool_call& call, const size_t index, const size_t count, const size_t tiles) {
  given_cells given;
  if (!call.has_input(index)) {
    return given;
  }
  given.field = call.input(index).read();
  given.present = true;
  // Диапазон у `sequential` покрывает только первый ВЫХОД, а входы читаются целиком — значит слишком
  // короткое поле условий диспетчер не поймает, и поймать его обязан инструмент. Иначе он читал бы
  // за концом буфера молча.
  if (given.field.count() < count) {
    utils::error{}("originator step '{}': tool '{}' got {} pre-taken cells for {} elements — a condition is "
                   "declared per cell, and a shorter field would be read past its end",
                   call.step_name, call.tool_name, given.field.count(), count);
  }
  for (size_t cell = 0; cell < count; ++cell) {
    const auto declared = size_t(given.at(cell));
    if (declared != 0 && declared - 1 >= tiles) {
      utils::error{}("originator step '{}': tool '{}' was given tile {} at element {}, but only {} tiles are "
                     "declared (0 means the cell is free, so the value is the tile plus one)",
                     call.step_name, call.tool_name, declared - 1, cell, tiles);
    }
  }
  return given;
}

void write_summary(const tool_call& call, const solve_report& report) {
  if (call.has_output(1)) {
    call.output(1).write().set(0, double(report.attempts));
  }
  if (call.has_output(2)) {
    call.output(2).write().set(0, double(report.rollbacks));
  }
}

[[noreturn]] void refuse_unsolved(const tool_call& call, const solve_report& report, const size_t attempts,
                                  const std::string& shape) {
  utils::error{}("originator step '{}': tool '{}' contradicted itself on all {} attempts over {} (rollbacks spent "
                 "{}) — the rules admit no assignment reachable this way, and a half-filled grid is not an answer",
                 call.step_name, call.tool_name, attempts, shape, report.rollbacks);
}

void tool_collapse(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': tool '{}' solves the WHOLE grid, so its range must start at 0, got [{}, {})",
                   call.step_name, call.tool_name, begin, end);
  }

  const auto& rules = *static_cast<const collapse_rules*>(call.shared);
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

  const auto attempts = size_t(std::max<int64_t>(call.params->integer("attempts", 16), 1));
  const auto given = read_given(call, 2, count, rules.tiles);

  raster_neighbours neighbours{width, height, call.params->number("wrap", 0.0) != 0.0};
  collapse_solver<raster_neighbours> solver(call, rules, count, neighbours, read_rollback_limits(call));
  const auto report = solver.run(given, attempts);
  if (!report.solved) {
    refuse_unsolved(call, report, attempts, std::format("a {}x{} raster", width, height));
  }

  auto target = call.output(0).write();
  for (size_t cell = 0; cell < count; ++cell) {
    target.set(cell, double(solver.tile_at(cell)));
  }
  write_summary(call, report);
}

void tool_graph_collapse(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': tool '{}' walks the WHOLE graph, so its range must start at 0, got [{}, {})",
                   call.step_name, call.tool_name, begin, end);
  }

  const auto& rules = *static_cast<const collapse_rules*>(call.shared);
  const size_t count = end > begin ? end - begin : 0;
  if (count == 0) {
    return;
  }

  const auto offsets = call.input(0).read();
  if (offsets.count() < count + 1) {
    utils::error{}("originator step '{}': tool '{}' needs {} offsets for {} cells (CSR keeps one past the end), "
                   "and '{}.{}' holds {}",
                   call.step_name, call.tool_name, count + 1, count,
                   call.input(0).buffer_name(), call.input(0).field_name(), offsets.count());
  }

  const auto attempts = size_t(std::max<int64_t>(call.params->integer("attempts", 16), 1));
  const auto given = read_given(call, 4, count, rules.tiles);

  graph_neighbours neighbours{offsets, call.input(1).read(), count};
  collapse_solver<graph_neighbours> solver(call, rules, count, neighbours, read_rollback_limits(call));
  const auto report = solver.run(given, attempts);
  if (!report.solved) {
    refuse_unsolved(call, report, attempts, std::format("a graph of {} cells", count));
  }

  auto target = call.output(0).write();
  for (size_t cell = 0; cell < count; ++cell) {
    target.set(cell, double(solver.tile_at(cell)));
  }
  write_summary(call, report);
}

// ПРАВИЛА ИЗ ОБРАЗЦА.
//
// Объявлять правила руками можно, пока их десяток. Дальше проще НАРИСОВАТЬ, как должно быть, и снять
// правила с рисунка — этим WFC и известен. Здесь окно `window x window` пробегает образец, РАЗНЫЕ
// окна становятся алфавитом узоров, а встреченные соседства окон — таблицей.
//
// ПРАВИЛО ОДНО НА ВСЕ РАЗМЕРЫ ОКНА: пара допущена тогда и только тогда, когда она ВСТРЕТИЛАСЬ в
// образце. Классика для окна больше единицы делает иначе — допускает пару, если окна СОГЛАСУЮТСЯ в
// перекрытии, даже если рядом их никто не видел. Разница существенна и названа:
//
//   согласование  даёт больше разнообразия и часто собирается с первой попытки;
//   встреченность даёт ПРОВЕРЯЕМОЕ обещание — «ни одной пары соседей, которой не было в образце», —
//                 и оно проверяется тестом и площадкой.
//
// Выбрана встреченность, потому что второе обещание можно предъявить, а первое нет. Цена названа:
// правила выходят теснее, противоречия чаще, и ровно за этим у решателя есть откаты. При окне 1 обе
// модели совпадают, и это ровно «простая табличная модель» — соседства тайлов, снятые с картинки.
//
// СИММЕТРИИ (повороты и отражения образца) НЕ добавляются намеренно: они умножают алфавит, а автор,
// которому они нужны, поворачивает образец сам и получает ровно то, что хотел.
struct learned_patterns {
  size_t positions_x = 0;
  size_t positions_y = 0;
  std::vector<uint32_t> at_position;
  std::vector<double> counts;
  std::vector<uint32_t> representative;
  size_t distinct = 0;
};

void tool_learn_rules(const tool_call& call, const size_t begin, const size_t end) {
  if (begin != 0) {
    utils::error{}("originator step '{}': tool '{}' reads the WHOLE sample, so its range must start at 0, "
                   "got [{}, {})", call.step_name, call.tool_name, begin, end);
  }

  const auto sample = call.input(0).read();
  const auto shape = resolve_extent(call, call.input(0), "width", "height");
  const size_t width = shape.x;
  const size_t height = shape.y == 0 ? 1 : shape.y;
  const size_t count = end > begin ? end - begin : 0;
  if (width * height != count) {
    utils::error{}("originator step '{}': tool '{}' got a range of {} elements over a {}x{} sample",
                   call.step_name, call.tool_name, count, width, height);
  }
  if (count == 0) {
    return;
  }

  const auto window = size_t(std::max<int64_t>(call.params->integer("window", 1), 1));
  const bool wrap = call.params->number("wrap", 0.0) != 0.0;
  const bool symmetric = call.params->number("symmetric", 0.0) != 0.0;

  // Симметричная таблица нужна ГРАФУ, а у графа нет ни оси, ни поворота. Окно больше единицы —
  // двумерное понятие, и снятые с него правила по построению зависят от направления. Совмещать одно
  // с другим значило бы объявить симметричным то, что симметричным не является.
  if (symmetric && window > 1) {
    utils::error{}("originator step '{}': tool '{}' was asked for symmetric rules with a {}x{} window — a window "
                   "larger than one cell is a directed statement about a raster, and a graph arc has no direction. "
                   "Learn with window = 1 for a graph, or drop 'symmetric' for a raster",
                   call.step_name, call.tool_name, window, window);
  }
  if (!wrap && (window > width || window > height)) {
    utils::error{}("originator step '{}': tool '{}' cannot fit a {}x{} window into a {}x{} sample",
                   call.step_name, call.tool_name, window, window, width, height);
  }

  learned_patterns learned;
  learned.positions_x = wrap ? width : width - window + 1;
  learned.positions_y = wrap ? height : height - window + 1;
  const size_t positions = learned.positions_x * learned.positions_y;
  learned.at_position.assign(positions, 0);

  // Ключ узора — БАЙТЫ окна: узоры сравниваются целиком, а не по хешу, поэтому совпадение хешей
  // ничего не портит. Порядок номеров задан порядком обхода образца, и он один и тот же всегда.
  gtl::flat_hash_map<std::string, uint32_t> known;
  std::string key(window * window * sizeof(uint32_t), '\0');

  for (size_t py = 0; py < learned.positions_y; ++py) {
    for (size_t px = 0; px < learned.positions_x; ++px) {
      for (size_t j = 0; j < window; ++j) {
        for (size_t i = 0; i < window; ++i) {
          const size_t sx = (px + i) % width;
          const size_t sy = (py + j) % height;
          const auto value = uint32_t(sample.get(sy * width + sx));
          std::memcpy(key.data() + (j * window + i) * sizeof(uint32_t), &value, sizeof(value));
        }
      }

      const auto found = known.find(key);
      uint32_t index = 0;
      if (found != known.end()) {
        index = found->second;
      } else {
        index = uint32_t(learned.distinct);
        known.emplace(key, index);
        learned.distinct += 1;
        learned.counts.push_back(0.0);
        // ПРЕДСТАВИТЕЛЬ узора — тайл в его якоре (левом верхнем углу). Он и есть то, чем узор
        // раскладывается обратно в тайлы: решатель работает в алфавите УЗОРОВ, а карта нужна в
        // тайлах, и перевод делается обычным `lookup` — второго механизма для этого не нужно.
        learned.representative.push_back(uint32_t(sample.get((py % height) * width + (px % width))));
      }
      learned.counts[index] += 1.0;
      learned.at_position[py * learned.positions_x + px] = index;
    }
  }

  auto weights_out = call.output(0).write();
  auto allowed_out = call.output(1).write();
  auto representative_out = call.output(2).write();

  const size_t capacity = weights_out.count();
  const size_t axes = symmetric ? 1 : primary_axes;
  if (learned.distinct > capacity) {
    utils::error{}("originator step '{}': tool '{}' found {} distinct {}x{} patterns in the sample, and '{}.{}' "
                   "declares room for {} — a generator must be able to name its cost before it runs, so declare "
                   "the capacity you actually need instead of getting a truncated alphabet",
                   call.step_name, call.tool_name, learned.distinct, window, window,
                   call.output(0).buffer_name(), call.output(0).field_name(), capacity);
  }
  if (allowed_out.count() < axes * capacity * capacity) {
    utils::error{}("originator step '{}': tool '{}' needs a rule matrix of {} elements for {} patterns ({} {}), "
                   "and '{}.{}' holds {}",
                   call.step_name, call.tool_name, axes * capacity * capacity, capacity, axes,
                   symmetric ? "axis, because a graph arc has no direction" : "axes",
                   call.output(1).buffer_name(), call.output(1).field_name(), allowed_out.count());
  }
  if (representative_out.count() < capacity) {
    utils::error{}("originator step '{}': tool '{}' needs {} representative tiles, and '{}.{}' holds {}",
                   call.step_name, call.tool_name, capacity,
                   call.output(2).buffer_name(), call.output(2).field_name(), representative_out.count());
  }

  // Незанятые места алфавита получают НУЛЕВОЙ вес, и решатель понимает это как «никогда»: свободная
  // ёмкость не превращается в узор, которого в образце не было.
  for (size_t i = 0; i < capacity; ++i) {
    weights_out.set(i, i < learned.distinct ? learned.counts[i] : 0.0);
    representative_out.set(i, i < learned.distinct ? double(learned.representative[i]) : 0.0);
  }
  for (size_t i = 0; i < axes * capacity * capacity; ++i) {
    allowed_out.set(i, 0.0);
  }

  const auto observe = [&](const size_t axis, const uint32_t a, const uint32_t b) {
    if (symmetric) {
      // Одна матрица и обе стороны: у дуги нет направления, поэтому «a рядом с b» и «b рядом с a» —
      // одно утверждение, и записать его надо один раз в обе клетки.
      allowed_out.set(size_t(a) * capacity + size_t(b), 1.0);
      allowed_out.set(size_t(b) * capacity + size_t(a), 1.0);
      return;
    }
    allowed_out.set((axis * capacity + size_t(a)) * capacity + size_t(b), 1.0);
  };

  for (size_t py = 0; py < learned.positions_y; ++py) {
    for (size_t px = 0; px < learned.positions_x; ++px) {
      const auto own = learned.at_position[py * learned.positions_x + px];
      if (px + 1 < learned.positions_x || wrap) {
        const size_t nx = (px + 1) % learned.positions_x;
        observe(0, own, learned.at_position[py * learned.positions_x + nx]);
      }
      if (py + 1 < learned.positions_y || wrap) {
        const size_t ny = (py + 1) % learned.positions_y;
        observe(1, own, learned.at_position[ny * learned.positions_x + px]);
      }
    }
  }

  if (call.has_output(3)) {
    call.output(3).write().set(0, double(learned.distinct));
  }
}

} // namespace

void tool_registry::add_constraint_tools() {
  const auto add = [this](tool_description description) { this->add(std::move(description)); };

  add(tool_description{
    .name = "collapse", .shape = aperture::sequential,
    .input_count = 3, .optional_inputs = 1, .output_count = 3, .optional_outputs = 2,
    .body = tool_collapse, .prepare = prepare_raster_rules, .footprint = collapse_footprint});

  add(tool_description{
    .name = "graph_collapse", .shape = aperture::sequential,
    .input_count = 5, .optional_inputs = 1, .output_count = 3, .optional_outputs = 2,
    .body = tool_graph_collapse, .prepare = prepare_graph_rules});

  // Апертура scatter, и по той же причине, что у `label_adjacency`: диапазон описывает ОБРАЗЕЦ, а
  // выходы — другую структуру (алфавит узоров и таблицу правил), размер которой задаёт объявленная
  // ёмкость, а не число прочитанных клеток. В очередь он не попадает: номера узоров раздаются в
  // порядке обхода, и это порядок, а не просто чужие индексы.
  add(tool_description{
    .name = "learn_rules", .shape = aperture::scatter,
    .input_count = 1, .output_count = 4, .optional_outputs = 1,
    .body = tool_learn_rules});
}

} // namespace originator
} // namespace devils_engine
