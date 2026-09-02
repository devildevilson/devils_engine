#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <queue>
#include <vector>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/kd_tree.h"

// Инструменты замкнутой поверхности и графа соседства.
//
// Зачем они существуют отдельно от вороного из примитивов: jc_voronoi живёт в ПЛОСКОСТИ, а у планеты
// плоскости нет. Любая развёртка сферы даёт шов, а шов — это место, где соседство считается неверно,
// причём тихо. Поэтому здесь адрес поверхности — направление, соседство — данные (CSR), и ни одна
// функция не знает, где у карты «край», потому что края нет.
//
// Второе, что здесь появляется, — словарь ГРАФА: размытие по соседям и заливка от нескольких
// источников. Он нужен потому, что на замкнутой поверхности почти всякий пространственный проход
// выражается через соседство, а не через окно растра: box_blur с радиусом в клетках на сфере
// неприменим вовсе.
//
// Апертуры выбраны по тому же правилу, что и у остальных инструментов:
//   sphere_points     pointwise — точка решётки зависит только от своего индекса;
//   sphere_adjacency  scatter   — пишет CSR, то есть по чужим индексам; детерминизм даёт алгоритм;
//   graph_blur        gather    — читает соседей, пишет свой элемент;
//   graph_flood       sequential — порядок обхода и ЕСТЬ смысл алгоритма.

namespace devils_engine {
namespace originator {

namespace {

using tree_point = std::array<float, 3>;
using cell_tree = utils::kd_tree<uint32_t, tree_point, 3>;

// Равнораспределённая решётка Фибоначчи на сфере.
//
// Выбрана не за красоту: у неё площадь ячейки почти одинакова по всей поверхности, поэтому
// «площадь = 4πR²/N» верно с точностью до процентов и любую сумму по клеткам можно считать без
// весов. У широтно-долготной сетки это неверно грубо (у полюса клетки в разы меньше), а у кубосферы
// — умеренно, но с шестью швами.
//
// Ось решётки — параметр: у планеты это ось вращения, и всё остальное (широта, инсоляция, кориолис)
// считается от неё. По умолчанию Y, как мировая вертикаль движка.
void tool_sphere_points(const tool_call& call, const size_t begin, const size_t end) {
  const auto& out = call.output(0);
  auto target = out.write();

  if (target.type().components < 3) {
    utils::error{}("originator step '{}': sphere_points needs a 3-component field, '{}.{}' has {}",
                   call.step_name, out.buffer_name(), out.field_name(), target.type().components);
  }

  const auto total = size_t(std::max<int64_t>(call.params->integer("count", int64_t(target.count())), 1));
  const double radius = call.params->number("radius", 1.0);
  const auto up = size_t(std::clamp<int64_t>(call.params->integer("axis", 1), 0, 2));
  const size_t first = (up + 1) % 3;
  const size_t second = (up + 2) % 3;

  // Обратное золотое сечение. Угол берётся от ДРОБНОЙ части произведения: у индекса в миллионы
  // произведение теряет старшие биты мантиссы, и без fmod решётка к концу спирали заметно плывёт.
  constexpr double golden = 0.61803398874989484820;

  for (size_t i = begin; i < end; ++i) {
    const double along = 1.0 - (2.0 * double(i) + 1.0) / double(total);
    const double ring = std::sqrt(std::max(0.0, 1.0 - along * along));
    const double angle = 2.0 * std::numbers::pi * std::fmod(double(i) * golden, 1.0);

    target.set(i, radius * ring * std::cos(angle), uint32_t(first));
    target.set(i, radius * along, uint32_t(up));
    target.set(i, radius * ring * std::sin(angle), uint32_t(second));
  }
}

// Общая структура для соседства: дерево точек, построенное ОДИН раз до разбиения работы.
struct adjacency_index {
  cell_tree tree;
  double mean_radius = 1.0;
};

std::shared_ptr<void> prepare_adjacency(const tool_call& call) {
  const auto positions = call.input(0).read();
  if (positions.type().components < 3) {
    utils::error{}("originator step '{}': sphere_adjacency needs a 3-component position field, '{}.{}' has {}",
                   call.step_name, call.input(0).buffer_name(), call.input(0).field_name(),
                   positions.type().components);
  }

  auto index = std::make_shared<adjacency_index>();
  const size_t begin = call.range_begin;
  const size_t end = call.range_end;

  index->tree.reserve(end - begin);
  double radius_sum = 0.0;
  for (size_t i = begin; i < end; ++i) {
    const tree_point point{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))};
    radius_sum += std::sqrt(double(point[0]) * point[0] + double(point[1]) * point[1] + double(point[2]) * point[2]);
    index->tree.insert(point, uint32_t(i));
  }
  index->mean_radius = end > begin ? radius_sum / double(end - begin) : 1.0;

  // Параллельная сборка дерева даёт ту же раскладку узлов, что последовательная, — это контракт
  // самого kd_tree. Но полагаться на неё для детерминизма всё равно нельзя: при равных расстояниях
  // побеждает первый найденный, поэтому кандидаты ниже сортируются по паре (расстояние, индекс).
  if (call.pool != nullptr && call.pool->size() != 0) {
    index->tree.build_parallel(*call.pool);
  } else {
    index->tree.build();
  }

  return index;
}

struct candidate {
  double distance = 0.0;
  uint32_t cell = 0;

  bool operator<(const candidate& other) const noexcept {
    // Тай-брейк по индексу обязателен: без него порядок соседей зависел бы от формы дерева, то есть
    // от nth_element на равных ключах, то есть ни от чего осмысленного.
    return distance < other.distance || (distance == other.distance && cell < other.cell);
  }
};

// sphere_adjacency: позиции -> симметричный CSR соседства.
//
// Соседство на сфере не выводится из растра и не берётся из плоской диаграммы. Здесь оно строится по
// K ближайшим и СИММЕТРИЗУЕТСЯ: если i считает j соседом, то и j считает соседом i. Асимметричный
// граф соседства — источник ошибок, которые проявляются через несколько шагов: заливка проходит в
// одну сторону, а размытие усредняет не то, и найти это по результату почти нельзя.
void tool_sphere_adjacency(const tool_call& call, const size_t begin, const size_t end) {
  const auto* index = static_cast<const adjacency_index*>(call.shared);
  if (index == nullptr) {
    utils::error{}("originator step '{}': sphere_adjacency lost its prepared index", call.step_name);
  }

  if (begin != 0) {
    utils::error{}("originator step '{}': sphere_adjacency builds the CSR of the WHOLE set, so its range must "
                   "start at 0, got [{}, {})", call.step_name, begin, end);
  }

  const auto positions = call.input(0).read();
  auto offsets = call.output(0).write();
  auto arcs = call.output(1).write();

  const size_t count = end - begin;
  if (offsets.count() < count + 1) {
    utils::error{}("originator step '{}': sphere_adjacency needs an offsets buffer of {} elements, got {}",
                   call.step_name, count + 1, offsets.count());
  }

  const auto neighbours = size_t(std::clamp<int64_t>(call.params->integer("neighbours", 6), 1, 32));
  const double scale = std::max(1.1, call.params->number("radius_scale", 2.6));

  // Среднее расстояние между точками равнораспределённой решётки: площадь ячейки 4πR²/N, значит
  // шаг порядка корня из неё. Радиус поиска берётся с запасом, а если кандидатов всё равно мало —
  // растёт, а не отдаёт неполный список.
  const double spacing = index->mean_radius * std::sqrt(4.0 * std::numbers::pi / double(std::max<size_t>(count, 1)));
  const double base_radius = scale * spacing;

  std::vector<uint32_t> nearest(count * neighbours, 0);
  std::vector<uint32_t> degrees(count, 0);

  const auto find_nearest = [&](const size_t first, const size_t last) {
    std::vector<candidate> found;
    found.reserve(neighbours * 4);

    for (size_t i = first; i < last; ++i) {
      const tree_point query{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))};

      double radius = base_radius;
      found.clear();
      for (uint32_t attempt = 0; attempt < 8 && found.size() < neighbours; ++attempt) {
        found.clear();
        index->tree.radius(
          query, float(radius), [](const uint32_t&) { return true; },
          [&](const cell_tree::node& node) {
            if (size_t(node.payload) == i) {
              return;
            }
            const double dx = double(node.pos[0]) - query[0];
            const double dy = double(node.pos[1]) - query[1];
            const double dz = double(node.pos[2]) - query[2];
            found.push_back(candidate{std::sqrt(dx * dx + dy * dy + dz * dz), node.payload});
          });
        radius *= 1.8;
      }

      std::sort(found.begin(), found.end());
      const size_t taken = std::min(found.size(), neighbours);
      degrees[i] = uint32_t(taken);
      for (size_t k = 0; k < taken; ++k) {
        nearest[i * neighbours + k] = found[k].cell;
      }
    }
  };

  if (call.pool != nullptr && call.pool->size() != 0 && count > 4096) {
    const size_t workers = call.pool->size() + 1;
    const size_t chunk = (count + workers - 1) / workers;
    for (size_t first = 0; first < count; first += chunk) {
      call.pool->submit(find_nearest, first, std::min(first + chunk, count));
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    find_nearest(0, count);
  }

  // Симметризация. Дуга кладётся в оба конца, затем список каждой клетки сортируется и чистится от
  // повторов — так CSR становится каноническим: он не зависит ни от порядка обхода, ни от того, кто
  // из двух соседей нашёл другого.
  std::vector<uint32_t> counts(count, 0);
  for (size_t i = 0; i < count; ++i) {
    for (uint32_t k = 0; k < degrees[i]; ++k) {
      const uint32_t other = nearest[i * neighbours + k];
      counts[i] += 1;
      counts[other] += 1;
    }
  }

  std::vector<size_t> starts(count + 1, 0);
  for (size_t i = 0; i < count; ++i) {
    starts[i + 1] = starts[i] + counts[i];
  }

  std::vector<uint32_t> filled(starts.back(), 0);
  std::vector<size_t> cursors(starts.begin(), starts.end() - 1);
  for (size_t i = 0; i < count; ++i) {
    for (uint32_t k = 0; k < degrees[i]; ++k) {
      const uint32_t other = nearest[i * neighbours + k];
      filled[cursors[i]++] = other;
      filled[cursors[other]++] = uint32_t(i);
    }
  }

  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    auto* first = filled.data() + starts[i];
    auto* last = filled.data() + starts[i] + counts[i];
    std::sort(first, last);
    last = std::unique(first, last);

    offsets.set(i, double(total));
    for (auto* it = first; it != last; ++it) {
      if (total >= arcs.count()) {
        utils::error{}("originator step '{}': sphere_adjacency needs an arc buffer of more than {} elements; "
                       "with {} neighbours the symmetric graph does not fit",
                       call.step_name, arcs.count(), neighbours);
      }
      arcs.set(total, double(*it));
      ++total;
    }
  }
  offsets.set(count, double(total));
}

// graph_blur: усреднение значения по соседям в CSR.
//
// Прямая замена box_blur там, где окна растра нет. Вес себя и вес соседа разделены, потому что этим
// одним инструментом решаются две разные задачи: сглаживание рельефа (себя больше) и диффузия
// величины по поверхности (соседей больше).
void tool_graph_blur(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto values = call.input(2).read();
  auto target = call.output(0).write();

  const double self_weight = call.params->number("self_weight", 1.0);
  const double neighbour_weight = call.params->number("neighbour_weight", 1.0);

  for (size_t i = begin; i < end; ++i) {
    double sum = self_weight * values.get(i);
    double weight = self_weight;

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      sum += neighbour_weight * values.get(size_t(arcs.get(k)));
      weight += neighbour_weight;
    }

    target.set(i, weight > 0.0 ? sum / weight : values.get(i));
  }
}

// graph_flood: заливка от нескольких источников по графу.
//
// Это тот инструмент, которым на замкнутой поверхности делается почти всякое «разбить на области»:
// плиты растут от затравок, провинции — от центров, морские зоны — от точек в воде. Метка 0 значит
// «нет метки», поэтому метки считаются с единицы.
//
// Апертура sequential не уступка, а суть: результат — порядок присвоения. Очередь упорядочена по
// тройке (расстояние, метка, клетка), поэтому равные расстояния разрешаются одинаково всегда, а не
// как повезёт куче.
//
// Отдельный вход ПРОХОДИМОСТИ существует потому, что дорогая стоимость и запрет — разные вещи, и
// подменять второе первым нельзя. Провинция, залитая через пролив «по дорогой цене», получается
// РАЗОРВАННОЙ: после отсечения воды у неё остаются два куска на разных берегах, и заметно это не
// сразу, а на отрисовке границы. Непроходимая клетка не входит в очередь вовсе, поэтому область
// связна по построению, а не по удачно подобранной цене.
void tool_graph_flood(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto seeds = call.input(2).read();
  const auto costs = call.input(3).read();
  const auto passable = call.input(4).read();
  auto label_out = call.output(0).write();
  auto distance_out = call.output(1).write();

  if (begin != 0) {
    utils::error{}("originator step '{}': graph_flood walks the WHOLE graph, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const size_t count = end > begin ? end - begin : 0;
  const double unreached = call.params->number("unreached", -1.0);
  const auto capacity = size_t(std::max<int64_t>(call.params->integer("capacity", 0), 0));

  struct entry {
    double distance = 0.0;
    uint32_t label = 0;
    uint32_t cell = 0;

    // priority_queue отдаёт МАКСИМУМ, поэтому сравнение перевёрнуто: ближе — «больше».
    bool operator<(const entry& other) const noexcept {
      if (distance != other.distance) {
        return distance > other.distance;
      }
      if (label != other.label) {
        return label > other.label;
      }
      return cell > other.cell;
    }
  };

  std::priority_queue<entry> queue;
  std::vector<uint32_t> assigned(count, 0);
  std::vector<uint32_t> used;

  for (size_t i = begin; i < end; ++i) {
    label_out.set(i, 0.0);
    distance_out.set(i, unreached);

    const auto seed = uint32_t(seeds.get(i));
    if (seed != 0 && passable.get(i) != 0.0) {
      queue.push(entry{0.0, seed, uint32_t(i)});
      if (used.size() <= seed) {
        used.resize(seed + 1, 0);
      }
    }
  }

  while (!queue.empty()) {
    const entry current = queue.top();
    queue.pop();

    if (assigned[current.cell] != 0) {
      continue;
    }
    if (used.size() <= current.label) {
      used.resize(size_t(current.label) + 1, 0);
    }
    // Метка, выбравшая свой лимит, дальше не растёт — но клетка при этом НЕ занимается: её ещё
    // может забрать другая метка, у которой лимит остался.
    if (capacity != 0 && used[current.label] >= capacity) {
      continue;
    }

    assigned[current.cell] = current.label;
    used[current.label] += 1;
    label_out.set(current.cell, double(current.label));
    distance_out.set(current.cell, current.distance);

    const auto first = size_t(offsets.get(current.cell));
    const auto last = size_t(offsets.get(size_t(current.cell) + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other = uint32_t(arcs.get(k));
      if (other >= count || assigned[other] != 0 || passable.get(other) == 0.0) {
        continue;
      }
      const double step = costs.get(other);
      if (step < 0.0) {
        // Отрицательная стоимость превращает заливку в бесконечный цикл, поэтому это ошибка данных,
        // а не повод «взять по модулю».
        utils::error{}("originator step '{}': graph_flood got a negative cost {} at cell {}",
                       call.step_name, step, other);
      }
      queue.push(entry{current.distance + step, current.label, other});
    }
  }
}

// graph_frontier: клетки, у которых есть сосед с ДРУГОЙ меткой.
//
// Вопрос «где граница» задаётся на каждом шаге генератора планеты — границы плит, берег, рубежи
// провинций, фронтир государств, — и ответ на него один и тот же, потому что граница это свойство
// графа, а не картинки. Вместе с graph_flood отсюда же получается расстояние до границы: залить от
// фронтира и прочитать distance.
void tool_graph_frontier(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto labels = call.input(2).read();
  auto target = call.output(0).write();

  // Метка, которая границей не считается (обычно 0 = «нет метки»): иначе берегом оказался бы и
  // стык двух безымянных клеток.
  const double ignored = call.params->number("ignore", -1.0);

  for (size_t i = begin; i < end; ++i) {
    const double own = labels.get(i);
    if (own == ignored) {
      target.set(i, 0.0);
      continue;
    }

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    double frontier = 0.0;
    for (size_t k = first; k < last; ++k) {
      const double other = labels.get(size_t(arcs.get(k)));
      if (other != own && other != ignored) {
        frontier = 1.0;
        break;
      }
    }
    target.set(i, frontier);
  }
}

// poisson_seeds: выбор затравок, разнесённых не ближе min_distance.
//
// Нужен потому, что «взять каждую k-ю клетку» на маске не работает: под маской (суша, вода, годные
// для жизни клетки) индексы идут неравномерно, и затравки собираются в кучи. Здесь выбор идёт
// жадно по порядку (счёт по убыванию, при равных — индекс), и каждая принятая затравка ВЫКЛЮЧАЕТ
// всех кандидатов в своём радиусе. Порядок задан полностью, поэтому результат один и тот же всегда.
//
// Апертура sequential: принятая затравка меняет судьбу следующих. Это ровно тот случай, когда
// порядок и есть алгоритм, а не деталь исполнения.
void tool_poisson_seeds(const tool_call& call, const size_t begin, const size_t end) {
  const auto positions = call.input(0).read();
  const auto mask = call.input(1).read();
  const auto score = call.input(2).read();
  auto target = call.output(0).write();

  if (begin != 0) {
    utils::error{}("originator step '{}': poisson_seeds walks the WHOLE set, so its range must start at 0, "
                   "got [{}, {})", call.step_name, begin, end);
  }

  const size_t count = end > begin ? end - begin : 0;
  const auto target_count = size_t(std::max<int64_t>(call.params->integer("target_count", 0), 0));
  const auto limit = size_t(std::max<int64_t>(call.params->integer("limit", 0), 0));

  for (size_t i = 0; i < count; ++i) {
    target.set(i, 0.0);
  }

  // Кандидаты — только то, что прошло маску. Средний радиус считается по ним же: маска может
  // покрывать четверть планеты, и шаг решётки по всему множеству был бы не тот.
  std::vector<candidate> candidates;
  candidates.reserve(count);
  double radius_sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    if (mask.get(i) == 0.0) {
      continue;
    }
    const double x = positions.get(i, 0);
    const double y = positions.get(i, 1);
    const double z = positions.get(i, 2);
    radius_sum += std::sqrt(x * x + y * y + z * z);
    // Порядок — по убыванию счёта, поэтому в ключ идёт минус: сравнение candidate уже даёт
    // тай-брейк по индексу, и второго правила заводить не нужно.
    candidates.push_back(candidate{-score.get(i), uint32_t(i)});
  }

  if (candidates.empty()) {
    return;
  }

  const double mean_radius = radius_sum / double(candidates.size());
  double min_distance = call.params->number("min_distance", 0.0);
  if (min_distance <= 0.0) {
    // Целевое число затравок переводится в расстояние по площади, покрытой маской: доля маски от
    // всего множества и есть доля площади, потому что решётка равнораспределённая.
    const size_t wanted = target_count != 0 ? target_count : std::max<size_t>(candidates.size() / 64, 1);
    const double covered = 4.0 * std::numbers::pi * mean_radius * mean_radius *
                           (double(candidates.size()) / double(count));
    min_distance = call.params->number("distance_scale", 0.92) * std::sqrt(covered / double(wanted));
  }

  std::sort(candidates.begin(), candidates.end());

  cell_tree tree;
  tree.reserve(candidates.size());
  for (const auto& entry : candidates) {
    const size_t i = entry.cell;
    tree.insert(tree_point{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))},
                uint32_t(i));
  }
  tree.build();

  std::vector<uint8_t> blocked(count, 0);
  size_t accepted = 0;
  for (const auto& entry : candidates) {
    const size_t i = entry.cell;
    if (blocked[i] != 0) {
      continue;
    }
    if (limit != 0 && accepted >= limit) {
      break;
    }

    ++accepted;
    target.set(i, double(accepted));

    const tree_point point{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))};
    tree.radius(
      point, float(min_distance), [](const uint32_t&) { return true; },
      [&](const cell_tree::node& node) { blocked[node.payload] = 1; });
  }
}

// graph_slope: средний модуль разности со всеми соседями.
//
// Отличается от «отклонения от размытого» ровно тем, ради чего и заведён: наклонная ПЛОСКОСТЬ от
// своего размытия почти не отклоняется, поэтому по отклонению ровное нагорье и ровный склон выглядят
// одинаково. Средний перепад до соседей — это настоящий уклон, и по нему нагорье отличается от горной
// страны, а абиссальная равнина от фланга хребта.
//
// Величина выходит В ЕДИНИЦАХ ПОЛЯ НА ШАГ РЕШЁТКИ, то есть зависит от разрешения; приводить её к
// расстоянию — дело вызывающего, который один и знает шаг своей решётки.
void tool_graph_slope(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto values = call.input(2).read();
  auto target = call.output(0).write();

  const size_t count = values.count();
  for (size_t i = begin; i < end; ++i) {
    const double own = values.get(i);
    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));

    double total = 0.0;
    size_t taken = 0;
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other >= count) {
        continue;
      }
      total += std::abs(values.get(other) - own);
      ++taken;
    }

    target.set(i, taken == 0 ? 0.0 : total / double(taken));
  }
}

// lookup: значение из ДРУГОГО буфера по индексу, лежащему в поле.
//
// Косвенность — это то же самое отношение, что и дуга графа, только записанное одним числом вместо
// списка: «моя плита», «моя провинция», «моя культура». Без инструмента такой выборки любое свойство
// группы приходилось бы разносить по клеткам вручную из lua, то есть поэлементным обходом миллионов
// элементов — ровно тем, чего правило про lua запрещает.
//
// Апертура gather: читается чужой элемент, пишется свой. Поэтому источник обязан отличаться от
// приёмника, и это ровно та проверка, которая здесь нужна.
void tool_lookup(const tool_call& call, const size_t begin, const size_t end) {
  const auto indices = call.input(0).read();
  const auto source = call.input(1).read();
  auto target = call.output(0).write();

  // Смещение существует потому, что метки в генераторе считаются С ЕДИНИЦЫ (ноль значит «нет
  // метки»), а элементы буфера групп — с нуля. Молча вычитать единицу нельзя: не всякое поле
  // индексов — метка.
  const auto offset = call.params->integer("offset", 0);
  const double missing = call.params->number("missing", 0.0);
  const uint32_t components = std::min(target.type().components, source.type().components);

  for (size_t i = begin; i < end; ++i) {
    const auto raw = int64_t(indices.get(i)) + offset;
    if (raw < 0 || size_t(raw) >= source.count()) {
      for (uint32_t component = 0; component < target.type().components; ++component) {
        target.set(i, missing, component);
      }
      continue;
    }

    for (uint32_t component = 0; component < components; ++component) {
      target.set(i, source.get(size_t(raw), component), component);
    }
  }
}

// graph_vote: незанятая клетка перенимает метку сильнейшего соседа.
//
// Второй способ разбить поверхность на области, и он отвечает на другой вопрос, чем graph_flood.
// Заливка — это «кто ближе», один проход, геометрия. Голосование — это «кто напористее», много
// проходов, и между проходами вес МЕНЯЕТСЯ: население выросло, соседняя область стала сильнее,
// граница поехала. Так расселение и распространение культуры получаются процессом, а не диаграммой
// Вороного по центрам.
//
// Порядок между клетками не важен: клетка меняет только себя и читает соседей из ВХОДНОГО поля,
// поэтому проход остаётся параллельным gather'ом, а «время» задаётся числом проходов из тела шага.
void tool_graph_vote(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto labels = call.input(2).read();
  const auto weights = call.input(3).read();
  const auto mask = call.input(4).read();
  auto target = call.output(0).write();

  const double threshold = call.params->number("threshold", 0.0);

  for (size_t i = begin; i < end; ++i) {
    const double own = labels.get(i);
    if (own != 0.0 || mask.get(i) == 0.0) {
      target.set(i, own);
      continue;
    }

    double best_label = 0.0;
    double best_weight = threshold;
    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      const size_t other = size_t(arcs.get(k));
      const double other_label = labels.get(other);
      if (other_label == 0.0) {
        continue;
      }
      const double weight = weights.get(other);
      // Строгое «больше» плюс тай-брейк по меньшей метке: при равных весах результат обязан быть
      // одним и тем же, а не зависеть от того, в каком порядке лежат дуги.
      if (weight > best_weight || (weight == best_weight && best_label != 0.0 && other_label < best_label)) {
        best_weight = weight;
        best_label = other_label;
      }
    }

    target.set(i, best_label);
  }
}

// connected_components: связные куски подграфа под маской.
//
// Отвечает на вопрос, который поклеточной меткой не выражается вовсе: «этот земляной массив» — это
// не свойство клетки, а свойство СВЯЗНОСТИ. Метка «суша» есть у каждой клетки суши, но того, что
// Евразия и Америка это два разных куска, в ней нет; чтобы это узнать, надо обойти граф. То же
// нужно и океанам, и вообще любой иерархии географических названий: верхний её уровень — всегда
// связный кусок, а не значение поля.
//
// Апертура sequential: обход одного куска нельзя разбить на чанки, не согласовав их между собой, а
// согласование стоит дороже самого обхода.
//
// Нумерация идёт в порядке ПЕРВОГО ПОЯВЛЕНИЯ (по индексу первой клетки куска), поэтому результат не
// зависит ни от порядка обхода внутри куска, ни от числа потоков. Куски мельче min_size получают
// ноль, а нумерация после отбрасывания остаётся ПЛОТНОЙ: дырка в номерах сломала бы group_by, для
// которого номер это индекс корзины.
void tool_connected_components(const tool_call& call, const size_t begin, const size_t end) {
  const auto offsets = call.input(0).read();
  const auto arcs = call.input(1).read();
  const auto mask = call.input(2).read();
  auto target = call.output(0).write();

  if (begin != 0) {
    utils::error{}("originator step '{}': connected_components walks the WHOLE graph, so its range must start "
                   "at 0, got [{}, {})", call.step_name, begin, end);
  }

  const size_t count = end > begin ? end - begin : 0;
  const auto minimum = size_t(std::max<int64_t>(call.params->integer("min_size", 0), 0));

  std::vector<uint32_t> raw(count, 0);
  std::vector<size_t> sizes;
  std::vector<uint32_t> stack;

  for (size_t start = 0; start < count; ++start) {
    if (raw[start] != 0 || mask.get(start) == 0.0) {
      continue;
    }

    const auto label = uint32_t(sizes.size() + 1);
    sizes.push_back(0);
    stack.clear();
    stack.push_back(uint32_t(start));
    raw[start] = label;

    while (!stack.empty()) {
      const auto cell = stack.back();
      stack.pop_back();
      sizes.back() += 1;

      const auto first = size_t(offsets.get(cell));
      const auto last = size_t(offsets.get(size_t(cell) + 1));
      for (size_t k = first; k < last; ++k) {
        const auto other = uint32_t(arcs.get(k));
        if (other >= count || raw[other] != 0 || mask.get(other) == 0.0) {
          continue;
        }
        raw[other] = label;
        stack.push_back(other);
      }
    }
  }

  // Плотная перенумерация: номер сохраняется только у кусков не меньше порога.
  std::vector<uint32_t> renumbered(sizes.size() + 1, 0);
  uint32_t kept = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (sizes[i] >= minimum) {
      renumbered[i + 1] = ++kept;
    }
  }

  for (size_t i = 0; i < count; ++i) {
    target.set(i, double(renumbered[raw[i]]));
  }
}

// label_adjacency: CSR по МЕТКАМ, собранный из CSR по элементам.
//
// Нужен потому, что иерархия областей строится НЕ поклеточно. Если растить историческую область
// заливкой по клеткам, её граница ляжет где попало и разрежет провинции на части — а по условию
// задачи все границы обязаны совпадать с границами провинций, иначе надпись и выделение начинают
// спорить друг с другом. Правильный способ один: собрать граф соседства ПРОВИНЦИЙ и растить область
// по нему, а клетка получает свою область через lookup по номеру провинции. Тогда совпадение границ
// не проверяется, а выполняется по построению.
//
// Строка метки L лежит по индексу L, а строка 0 всегда пуста. Соглашение то же, что у group_by и
// accumulate: корзина равна СЫРОМУ значению ключа, а ключ 0 означает «метки нет». Один индекс на
// все таблицы областей — иначе запись области, её суммы и её строка соседства расходятся, и
// расхождение это ничем не ловится, потому что все три буфера остаются валидными.
//
// Граф выходит симметричным сам, без досимметризации: соседство элементов симметрично, а метка
// соседа не зависит от того, с какой стороны на дугу смотреть.
void tool_label_adjacency(const tool_call& call, const size_t begin, const size_t end) {
  const auto labels = call.input(0).read();
  const auto offsets = call.input(1).read();
  const auto arcs = call.input(2).read();
  auto row_offsets = call.output(0).write();
  auto row_arcs = call.output(1).write();

  if (begin != 0) {
    utils::error{}("originator step '{}': label_adjacency builds the CSR of the WHOLE label set, so its range "
                   "must start at 0, got [{}, {})", call.step_name, begin, end);
  }

  const size_t count = end > begin ? end - begin : 0;
  const size_t rows = row_offsets.count() == 0 ? 0 : row_offsets.count() - 1;
  if (rows == 0) {
    utils::error{}("originator step '{}': label_adjacency needs an offsets buffer of at least two elements, got {}",
                   call.step_name, row_offsets.count());
  }

  std::vector<std::vector<uint32_t>> neighbours(rows);
  for (size_t i = 0; i < count; ++i) {
    const auto own = uint32_t(labels.get(i));
    if (own == 0) {
      continue;
    }
    if (own >= rows) {
      utils::error{}("originator step '{}': label_adjacency got label {} at element {}, but the offsets buffer "
                     "holds only {} rows", call.step_name, own, i, rows);
    }

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    for (size_t k = first; k < last; ++k) {
      const auto other_element = size_t(arcs.get(k));
      if (other_element >= count) {
        continue;
      }
      const auto other = uint32_t(labels.get(other_element));
      if (other == 0 || other == own) {
        continue;
      }
      if (other >= rows) {
        utils::error{}("originator step '{}': label_adjacency got label {} at element {}, but the offsets buffer "
                       "holds only {} rows", call.step_name, other, other_element, rows);
      }
      neighbours[own].push_back(other);
    }
  }

  size_t total = 0;
  for (size_t row = 0; row < rows; ++row) {
    auto& list = neighbours[row];
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());

    row_offsets.set(row, double(total));
    for (const auto value : list) {
      if (total >= row_arcs.count()) {
        utils::error{}("originator step '{}': label_adjacency needs an arc buffer of more than {} elements",
                       call.step_name, row_arcs.count());
      }
      row_arcs.set(total, double(value));
      ++total;
    }
  }
  row_offsets.set(rows, double(total));
}

} // namespace

void tool_registry::add_graph_tools() {
  const auto add = [this](tool_description description) { this->add(std::move(description)); };

  add(tool_description{.name = "sphere_points", .shape = aperture::pointwise, .input_count = 0, .output_count = 1,
                       .body = tool_sphere_points});
  add(tool_description{.name = "sphere_adjacency", .shape = aperture::scatter, .input_count = 1, .output_count = 2,
                       .body = tool_sphere_adjacency, .prepare = prepare_adjacency});
  add(tool_description{.name = "graph_blur", .shape = aperture::gather, .input_count = 3, .output_count = 1,
                       .body = tool_graph_blur});
  add(tool_description{.name = "graph_flood", .shape = aperture::sequential, .input_count = 5, .output_count = 2,
                       .body = tool_graph_flood});
  add(tool_description{.name = "graph_frontier", .shape = aperture::gather, .input_count = 3, .output_count = 1,
                       .body = tool_graph_frontier});
  add(tool_description{.name = "poisson_seeds", .shape = aperture::sequential, .input_count = 3, .output_count = 1,
                       .body = tool_poisson_seeds});
  add(tool_description{.name = "graph_vote", .shape = aperture::gather, .input_count = 5, .output_count = 1,
                       .body = tool_graph_vote});
  add(tool_description{.name = "graph_slope", .shape = aperture::gather, .input_count = 3, .output_count = 1,
                       .body = tool_graph_slope});
  add(tool_description{.name = "lookup", .shape = aperture::gather, .input_count = 2, .output_count = 1,
                       .body = tool_lookup});
  add(tool_description{.name = "connected_components", .shape = aperture::sequential, .input_count = 3,
                       .output_count = 1, .body = tool_connected_components});
  add(tool_description{.name = "label_adjacency", .shape = aperture::scatter, .input_count = 3, .output_count = 2,
                       .body = tool_label_adjacency});
}

} // namespace originator
} // namespace devils_engine
