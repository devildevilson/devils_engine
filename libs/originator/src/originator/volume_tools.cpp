#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"
#include "devils_engine/utils/core.h"

// Объёмные инструменты: генератор, который отдаёт не поле, а ГЕОМЕТРИЮ.
//
// Это первый инструмент библиотеки, у которого длина выхода не равна длине входа и не известна до
// исполнения: сколько треугольников даст поле плотности, знает только само поле. Отдельного
// механизма под это не заведено, и заводить его не понадобилось — ёмкость объявлена размером буфера,
// а СКОЛЬКО занято, приезжает обычным буфером на один элемент, как и любое другое состояние между
// шагами. Переполнение объявленной ёмкости — громкий отказ с именами и числами, а не молчаливая
// обрезка: обрезанная поверхность выглядит как дырка в мире, и найти её причину по картинке нельзя.
//
// Апертура — scatter, потому что инструмент пишет по чужим индексам (клетка пишет в свой отрезок
// списка вершин). Детерминизм даёт не апертура, а схема: три фазы с ФИКСИРОВАННЫМ разбиением —
// подсчёт по клеткам, префиксная сумма в порядке клеток, запись каждой клеткой своего заранее
// вычисленного отрезка. Ровно тот же приём, из которого сделаны group_by и accumulate, поэтому
// результат побитово один и тот же при любом числе потоков.
//
// Носитель ключа здесь ЧАНКОВЫЙ, и это первый честный chunk_local в проекте: группа — это список
// вершин одного прохода, и чанк заканчивает её сам. Объявляет это, как и положено, автор пайплайна:
// key_support = "chunk_local" в вызове.

namespace devils_engine {
namespace originator {

namespace {

// Разбиение подсчёта и записи. Задано константой, а не числом потоков: от него зависит порядок
// вершин в выходе, поэтому оно не может зависеть от того, сколько ядер у машины.
constexpr size_t volume_chunk_size = 16384;

// Угол куба: c = dx + 2*dy + 4*dz. Номер угла — это тройка бит, поэтому переход по оси это
// переключение одного бита, а не таблица.
constexpr uint32_t corner_offset(const uint32_t corner, const uint32_t axis) noexcept {
  return (corner >> axis) & 1u;
}

// Двенадцать рёбер куба: сначала все вдоль x, потом вдоль y, потом вдоль z. Порядок нужен только для
// того, чтобы у ребра был устойчивый номер; таблица случаев ниже строится по этому же порядку.
struct cube_edges {
  std::array<std::array<uint8_t, 2>, 12> corners{};

  constexpr cube_edges() noexcept {
    size_t index = 0;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const uint32_t bit = 1u << axis;
      for (uint32_t corner = 0; corner < 8; ++corner) {
        if ((corner & bit) != 0) {
          continue;
        }
        corners[index][0] = uint8_t(corner);
        corners[index][1] = uint8_t(corner | bit);
        ++index;
      }
    }
  }

  constexpr uint32_t find(const uint32_t a, const uint32_t b) const noexcept {
    for (uint32_t i = 0; i < 12; ++i) {
      if ((corners[i][0] == a && corners[i][1] == b) || (corners[i][0] == b && corners[i][1] == a)) {
        return i;
      }
    }
    return 12;
  }
};

constexpr cube_edges edges{};

// ТАБЛИЦА СЛУЧАЕВ ВЫВОДИТСЯ, А НЕ ПЕРЕПИСЫВАЕТСЯ.
//
// Канонические 256 строк по 16 чисел — это данные, которые можно списать с опечаткой, и одна
// неверная строка означает дырку в поверхности при одном конкретном сочетании знаков, то есть баг,
// который видно раз в сотню чанков и который не воспроизводится «на глаз». Поэтому таблица строится
// из правила, и правило это то же самое, из которого её и получают:
//
//   1. ребро РАЗРЕЗАНО, если его концы разного знака: там и лежит вершина;
//   2. на каждой ГРАНИ куба контур — это отрезки между разрезанными рёбрами этой грани;
//   3. отрезки замыкаются в циклы, потому что у разрезанного ребра ровно две грани, и на каждой оно
//      конец ровно одного отрезка;
//   4. цикл разворачивается веером в треугольники.
//
// Главное свойство приходит из шага 2: правило соединения на грани зависит ТОЛЬКО от четырёх знаков
// самой грани. Два соседних куба видят эту грань одинаково, значит их контуры на общей грани
// совпадают по построению — поверхность непрерывна не потому, что так вышло, а потому, что иначе
// быть не может. На неоднозначной грани (знаки чередуются по кругу) отрезки отсекают ПЛОТНЫЕ углы:
// выбор произволен, но он тоже функция знаков, поэтому у соседа он тот же.
//
// Ориентация: отрезок направлен так, чтобы плотная часть грани оставалась СПРАВА при взгляде снаружи
// куба. Тогда циклы согласованы между собой, и веер даёт треугольники с нормалью в одну сторону —
// в сторону убывания плотности, то есть наружу от вещества. Это же проверяется тестом на сфере.
struct case_table {
  static constexpr size_t max_triangles = 5;
  static constexpr size_t row = max_triangles * 3;

  std::array<std::array<int8_t, row>, 256> triangles{};
  std::array<uint8_t, 256> counts{};

  case_table() {
    // Углы грани в обходе ПРОТИВ ЧАСОВОЙ при взгляде снаружи. Пара осей выбрана так, чтобы их
    // векторное произведение давало внешнюю нормаль, иначе обход пошёл бы в обратную сторону и вся
    // поверхность вывернулась бы наизнанку.
    struct face_description {
      uint32_t axis = 0;
      uint32_t value = 0;
      std::array<uint8_t, 4> corners{};
      std::array<float, 3> normal{};
    };

    std::array<face_description, 6> faces{};
    size_t face_index = 0;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      for (uint32_t value = 0; value < 2; ++value) {
        // (b, c, n) — правая тройка: b x c = n. Для внешней нормали -axis пара осей меняется местами.
        const uint32_t first = (axis + 1) % 3;
        const uint32_t second = (axis + 2) % 3;
        const uint32_t b = value == 1 ? first : second;
        const uint32_t c = value == 1 ? second : first;

        auto& face = faces[face_index++];
        face.axis = axis;
        face.value = value;
        face.normal[axis] = value == 1 ? 1.0f : -1.0f;

        static constexpr std::array<std::array<uint32_t, 2>, 4> ring{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (size_t k = 0; k < 4; ++k) {
          face.corners[k] = uint8_t((value << axis) | (ring[k][0] << b) | (ring[k][1] << c));
        }
      }
    }

    const auto corner_position = [](const uint32_t corner) {
      return std::array<float, 3>{float(corner_offset(corner, 0)), float(corner_offset(corner, 1)),
                                  float(corner_offset(corner, 2))};
    };

    for (uint32_t mask = 0; mask < 256; ++mask) {
      // Отрезки контура: from -> to по номерам разрезанных рёбер куба.
      std::array<uint8_t, 12> next{};
      std::array<bool, 12> has_next{};

      for (const auto& face : faces) {
        std::array<bool, 4> solid{};
        for (size_t k = 0; k < 4; ++k) {
          solid[k] = ((mask >> face.corners[k]) & 1u) != 0;
        }

        // Разрезанные рёбра грани в её собственном обходе: ребро k соединяет углы k и k+1.
        std::array<uint32_t, 4> cut{};
        size_t cut_count = 0;
        for (size_t k = 0; k < 4; ++k) {
          if (solid[k] != solid[(k + 1) % 4]) {
            cut[cut_count++] = uint32_t(k);
          }
        }

        if (cut_count == 0) {
          continue;
        }

        // Пары отрезков. Два разреза дают один отрезок; четыре — два, и тогда каждый отсекает свой
        // ПЛОТНЫЙ угол. Выбор произволен, но он функция знаков грани, поэтому сосед сделает такой же.
        std::array<std::array<uint32_t, 2>, 2> pairs{};
        size_t pair_count = 0;
        if (cut_count == 2) {
          pairs[pair_count++] = {cut[0], cut[1]};
        } else {
          // Чередование знаков: разрезаны все четыре ребра. Плотный угол лежит между рёбрами k-1 и k;
          // отсекаем каждый по отдельности.
          for (size_t k = 0; k < 4; ++k) {
            if (!solid[k]) {
              continue;
            }
            const uint32_t before = uint32_t((k + 3) % 4);
            const uint32_t after = uint32_t(k);
            pairs[pair_count++] = {before, after};
          }
          if (pair_count != 2) {
            utils::error{}("originator: marching cubes case {} has {} cuts on one face but {} solid "
                           "corners — the face rule is inconsistent",
                           mask, cut_count, pair_count);
          }
        }

        for (size_t p = 0; p < pair_count; ++p) {
          const uint32_t face_edge_a = pairs[p][0];
          const uint32_t face_edge_b = pairs[p][1];

          const auto midpoint = [&](const uint32_t face_edge) {
            const auto first = corner_position(face.corners[face_edge]);
            const auto second = corner_position(face.corners[(face_edge + 1) % 4]);
            return std::array<float, 3>{(first[0] + second[0]) * 0.5f, (first[1] + second[1]) * 0.5f,
                                        (first[2] + second[2]) * 0.5f};
          };

          const auto point_a = midpoint(face_edge_a);
          const auto point_b = midpoint(face_edge_b);

          // Плотная сторона отрезка. Для двух разрезов берётся середина всех плотных углов грани,
          // для четырёх — тот единственный угол, который этот отрезок отсекает: у чередующихся
          // знаков середина плотных углов лежит В ЦЕНТРЕ грани и не говорит ничего.
          std::array<float, 3> solid_point{};
          if (cut_count == 2) {
            size_t solid_count = 0;
            for (size_t k = 0; k < 4; ++k) {
              if (!solid[k]) {
                continue;
              }
              const auto position = corner_position(face.corners[k]);
              solid_point[0] += position[0];
              solid_point[1] += position[1];
              solid_point[2] += position[2];
              ++solid_count;
            }
            for (float& component : solid_point) {
              component /= float(solid_count);
            }
          } else {
            solid_point = corner_position(face.corners[face_edge_b]);
          }

          const std::array<float, 3> direction{point_b[0] - point_a[0], point_b[1] - point_a[1],
                                               point_b[2] - point_a[2]};
          // Справа от идущего по отрезку (вверх — внешняя нормаль грани): right = direction x normal.
          const std::array<float, 3> right{
            direction[1] * face.normal[2] - direction[2] * face.normal[1],
            direction[2] * face.normal[0] - direction[0] * face.normal[2],
            direction[0] * face.normal[1] - direction[1] * face.normal[0]};
          const std::array<float, 3> middle{(point_a[0] + point_b[0]) * 0.5f, (point_a[1] + point_b[1]) * 0.5f,
                                            (point_a[2] + point_b[2]) * 0.5f};
          const float side = (solid_point[0] - middle[0]) * right[0] + (solid_point[1] - middle[1]) * right[1] +
                             (solid_point[2] - middle[2]) * right[2];

          const auto cube_edge_of = [&](const uint32_t face_edge) {
            return edges.find(face.corners[face_edge], face.corners[(face_edge + 1) % 4]);
          };

          const uint32_t from = side >= 0.0f ? cube_edge_of(face_edge_a) : cube_edge_of(face_edge_b);
          const uint32_t to = side >= 0.0f ? cube_edge_of(face_edge_b) : cube_edge_of(face_edge_a);
          if (from >= 12 || to >= 12) {
            utils::error{}("originator: marching cubes case {} produced an unknown cube edge", mask);
          }
          if (has_next[from]) {
            utils::error{}("originator: marching cubes case {} gives cube edge {} two outgoing "
                           "segments — the contour is not a set of cycles",
                           mask, from);
          }
          next[from] = uint8_t(to);
          has_next[from] = true;
        }
      }

      // Циклы веером в треугольники. Обход начинается с наименьшего номера ребра, поэтому порядок
      // треугольников в строке таблицы определён однозначно и не зависит ни от чего внешнего.
      std::array<bool, 12> visited{};
      size_t written = 0;
      auto& row_data = triangles[mask];
      row_data.fill(-1);

      for (uint32_t start = 0; start < 12; ++start) {
        if (!has_next[start] || visited[start]) {
          continue;
        }

        std::array<uint8_t, 12> loop{};
        size_t length = 0;
        uint32_t current = start;
        while (!visited[current]) {
          visited[current] = true;
          loop[length++] = uint8_t(current);
          current = next[current];
          if (!has_next[current] && current != start) {
            utils::error{}("originator: marching cubes case {} has a broken contour at edge {}", mask, current);
          }
        }
        if (current != start) {
          utils::error{}("originator: marching cubes case {} produced a contour that does not close", mask);
        }
        if (length < 3) {
          utils::error{}("originator: marching cubes case {} produced a contour of {} edges", mask, length);
        }

        for (size_t k = 1; k + 1 < length; ++k) {
          if (written + 3 > row) {
            utils::error{}("originator: marching cubes case {} needs more than {} triangles", mask, max_triangles);
          }
          row_data[written++] = int8_t(loop[0]);
          row_data[written++] = int8_t(loop[k]);
          row_data[written++] = int8_t(loop[k + 1]);
        }
      }

      counts[mask] = uint8_t(written / 3);
    }
  }
};

const case_table& cases() {
  // Таблица одна на процесс и неизменна после построения: строится она из правила за микросекунды,
  // но строить её в каждом чанке было бы платой ни за что.
  static const case_table table;
  return table;
}

// Разобранные параметры вызова. Собраны в одном месте, потому что их читают и фаза подсчёта, и фаза
// записи, и разъехавшиеся между фазами размеры дали бы вершины не в тех клетках.
struct volume_settings {
  size_t size[3]{1, 1, 1};
  size_t first[3]{0, 0, 0}; // первая клетка по оси
  size_t cells[3]{0, 0, 0}; // число клеток по оси
  size_t cell_count = 0;
  double iso = 0.0;
  size_t border = 0;
};

volume_settings read_settings(const tool_call& call) {
  volume_settings settings;
  const auto& p = *call.params;

  settings.size[0] = size_t(std::max<int64_t>(p.integer("size_x", 1), 1));
  settings.size[1] = size_t(std::max<int64_t>(p.integer("size_y", int64_t(settings.size[0])), 1));
  settings.size[2] = size_t(std::max<int64_t>(p.integer("size_z", int64_t(settings.size[1])), 1));
  settings.iso = p.number("iso", 0.0);
  settings.border = size_t(std::max<int64_t>(p.integer("border", 0), 0));

  const size_t declared = settings.size[0] * settings.size[1] * settings.size[2];
  if (declared != call.range_count()) {
    utils::error{}("originator step '{}': marching_cubes was told the grid is {}x{}x{} = {} samples, but "
                   "the range covers {} — the grid and the buffer must be the same thing",
                   call.step_name, settings.size[0], settings.size[1], settings.size[2], declared,
                   call.range_count());
  }

  settings.cell_count = 1;
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const size_t needed = 2 * settings.border + 2;
    if (settings.size[axis] < needed) {
      utils::error{}("originator step '{}': marching_cubes needs at least {} samples along axis {} for "
                     "border {}, got {}",
                     call.step_name, needed, axis, settings.border, settings.size[axis]);
    }
    settings.first[axis] = settings.border;
    settings.cells[axis] = settings.size[axis] - 1 - 2 * settings.border;
    settings.cell_count *= settings.cells[axis];
  }

  return settings;
}

// Что инструмент читает и пишет. Поля разрешаются один раз: в горячем цикле остаются аксессоры.
struct volume_bindings {
  const_field_accessor density;
  const_field_accessor position;
  field_accessor out_position;
  field_accessor out_normal;
  field_accessor out_count;
  std::span<const float> density_span;
  size_t capacity = 0;
};

volume_bindings read_bindings(const tool_call& call) {
  volume_bindings bound;

  const auto& density_ref = call.input(0);
  const auto& position_ref = call.input(1);
  if (density_ref.type().components != 1) {
    utils::error{}("originator step '{}': marching_cubes needs a single-component density field, '{}.{}' has {}",
                   call.step_name, density_ref.buffer_name(), density_ref.field_name(),
                   density_ref.type().components);
  }
  if (position_ref.type().components < 3) {
    utils::error{}("originator step '{}': marching_cubes needs a 3-component position field, '{}.{}' has {}",
                   call.step_name, position_ref.buffer_name(), position_ref.field_name(),
                   position_ref.type().components);
  }

  const auto& out_position_ref = call.output(0);
  const auto& out_normal_ref = call.output(1);
  const auto& out_count_ref = call.output(2);
  if (out_position_ref.type().components < 3 || out_normal_ref.type().components < 3) {
    utils::error{}("originator step '{}': marching_cubes writes 3-component vertex position and normal, "
                   "got '{}.{}' with {} and '{}.{}' with {}",
                   call.step_name, out_position_ref.buffer_name(), out_position_ref.field_name(),
                   out_position_ref.type().components, out_normal_ref.buffer_name(),
                   out_normal_ref.field_name(), out_normal_ref.type().components);
  }
  if (out_position_ref.count() != out_normal_ref.count()) {
    utils::error{}("originator step '{}': marching_cubes writes vertices into '{}' ({} elements) and '{}' "
                   "({} elements) — position and normal must be the same buffer length",
                   call.step_name, out_position_ref.buffer_name(), out_position_ref.count(),
                   out_normal_ref.buffer_name(), out_normal_ref.count());
  }
  if (out_count_ref.count() == 0) {
    utils::error{}("originator step '{}': marching_cubes needs a buffer of at least one element for the "
                   "vertex count, '{}' has none",
                   call.step_name, out_count_ref.buffer_name());
  }

  bound.density = density_ref.read();
  bound.position = position_ref.read();
  bound.out_position = out_position_ref.write();
  bound.out_normal = out_normal_ref.write();
  bound.out_count = out_count_ref.write();
  bound.density_span = bound.density.as_span<float>();
  bound.capacity = out_position_ref.count();
  return bound;
}

struct volume_grid {
  const volume_settings* settings = nullptr;
  const volume_bindings* bound = nullptr;

  size_t sample_index(const size_t x, const size_t y, const size_t z) const noexcept {
    return x + settings->size[0] * (y + settings->size[1] * z);
  }

  double density_at(const size_t index) const noexcept {
    return bound->density_span.empty() ? bound->density.get(index) : double(bound->density_span[index]);
  }

  // Градиент плотности в узле решётки. Центральная разность там, где сосед есть с обеих сторон, и
  // односторонняя на самом краю сетки. Именно поэтому чанковому проходу нужна полоса в один отсчёт с
  // каждой стороны: с ней у КАЖДОГО угла клетки центральная разность, значит нормаль на общей грани
  // двух чанков считается по одним и тем же числам и совпадает точно, а не приблизительно.
  std::array<double, 3> gradient_at(const size_t x, const size_t y, const size_t z) const noexcept {
    const std::array<size_t, 3> coordinate{x, y, z};
    std::array<double, 3> result{};

    for (uint32_t axis = 0; axis < 3; ++axis) {
      std::array<size_t, 3> lower = coordinate;
      std::array<size_t, 3> upper = coordinate;
      lower[axis] = coordinate[axis] > 0 ? coordinate[axis] - 1 : coordinate[axis];
      upper[axis] = coordinate[axis] + 1 < settings->size[axis] ? coordinate[axis] + 1 : coordinate[axis];

      const size_t low_index = sample_index(lower[0], lower[1], lower[2]);
      const size_t high_index = sample_index(upper[0], upper[1], upper[2]);
      const double distance = bound->position.get(high_index, axis) - bound->position.get(low_index, axis);
      const double difference = density_at(high_index) - density_at(low_index);
      result[axis] = distance != 0.0 ? difference / distance : 0.0;
    }

    return result;
  }
};

// Знак угла. Плотное вещество — там, где плотность НЕ МЕНЬШЕ порога; на самом пороге угол считается
// плотным, иначе поле, ровно равное порогу на целой плоскости, дало бы поверхность из ничего.
bool is_solid(const double value, const double iso) noexcept {
  return value >= iso;
}

uint32_t cube_mask(const volume_grid& grid, const size_t x, const size_t y, const size_t z) noexcept {
  uint32_t mask = 0;
  for (uint32_t corner = 0; corner < 8; ++corner) {
    const size_t index = grid.sample_index(x + corner_offset(corner, 0), y + corner_offset(corner, 1),
                                           z + corner_offset(corner, 2));
    if (is_solid(grid.density_at(index), grid.settings->iso)) {
      mask |= 1u << corner;
    }
  }
  return mask;
}

void tool_marching_cubes(const tool_call& call, const size_t, const size_t) {
  const auto settings = read_settings(call);
  const auto bound = read_bindings(call);
  const auto& table = cases();

  volume_grid grid;
  grid.settings = &settings;
  grid.bound = &bound;

  const auto cell_coordinate = [&](const size_t cell) {
    const size_t x = cell % settings.cells[0];
    const size_t y = (cell / settings.cells[0]) % settings.cells[1];
    const size_t z = cell / (settings.cells[0] * settings.cells[1]);
    return std::array<size_t, 3>{settings.first[0] + x, settings.first[1] + y, settings.first[2] + z};
  };

  const size_t chunks = settings.cell_count == 0 ? 0 : (settings.cell_count + volume_chunk_size - 1) / volume_chunk_size;

  // Фаза 1: случай куба на клетку. Знаки нужны и подсчёту, и записи, поэтому считаются один раз:
  // второй проход по плотности стоил бы столько же, сколько сама фаза.
  std::vector<uint8_t> masks(settings.cell_count, 0);
  std::vector<uint32_t> offsets(settings.cell_count + 1, 0);

  const auto classify_chunk = [&](const size_t chunk) {
    const size_t first = chunk * volume_chunk_size;
    const size_t last = std::min(first + volume_chunk_size, settings.cell_count);
    for (size_t cell = first; cell < last; ++cell) {
      const auto at = cell_coordinate(cell);
      const uint32_t mask = cube_mask(grid, at[0], at[1], at[2]);
      masks[cell] = uint8_t(mask);
      offsets[cell] = uint32_t(table.counts[mask]) * 3u;
    }
  };

  const bool parallel = call.pool != nullptr && call.pool->size() != 0 && chunks > 1;
  if (parallel) {
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
      call.pool->submit([&classify_chunk](const size_t index) { classify_chunk(index); }, chunk);
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
      classify_chunk(chunk);
    }
  }

  // Фаза 2: префиксная сумма в порядке клеток. Целочисленная и последовательная, поэтому отрезок
  // каждой клетки один и тот же при любом раскладе исполнения.
  size_t running = 0;
  for (size_t cell = 0; cell < settings.cell_count; ++cell) {
    const uint32_t count = offsets[cell];
    offsets[cell] = uint32_t(running);
    running += count;
  }
  offsets[settings.cell_count] = uint32_t(running);

  if (running > bound.capacity) {
    utils::error{}("originator step '{}': marching_cubes produced {} vertices, but '{}' declares room for "
                   "{} — raise the declared capacity or generate a smaller cell block; a silently "
                   "truncated surface reads as a hole in the world",
                   call.step_name, running, call.output(0).buffer_name(), bound.capacity);
  }

  // Фаза 3: запись. Каждая клетка пишет в свой отрезок, отрезки не пересекаются, поэтому ни атомиков,
  // ни порядка здесь нет — и результат не зависит от того, кто когда успел.
  const auto emit_chunk = [&](const size_t chunk) {
    const size_t first = chunk * volume_chunk_size;
    const size_t last = std::min(first + volume_chunk_size, settings.cell_count);

    for (size_t cell = first; cell < last; ++cell) {
      const uint32_t mask = masks[cell];
      const uint8_t triangle_count = table.counts[mask];
      if (triangle_count == 0) {
        continue;
      }

      const auto at = cell_coordinate(cell);
      const auto& row = table.triangles[mask];
      size_t cursor = offsets[cell];

      // Вершины ребра куба считаются по требованию, но один раз на клетку: у случая с пятью
      // треугольниками одно ребро встречается до трёх раз.
      std::array<bool, 12> ready{};
      std::array<std::array<double, 3>, 12> edge_position{};
      std::array<std::array<double, 3>, 12> edge_normal{};

      const auto prepare_edge = [&](const uint32_t edge) {
        if (ready[edge]) {
          return;
        }
        ready[edge] = true;

        const uint32_t low_corner = edges.corners[edge][0];
        const uint32_t high_corner = edges.corners[edge][1];
        const std::array<size_t, 3> low{at[0] + corner_offset(low_corner, 0), at[1] + corner_offset(low_corner, 1),
                                        at[2] + corner_offset(low_corner, 2)};
        const std::array<size_t, 3> high{at[0] + corner_offset(high_corner, 0),
                                         at[1] + corner_offset(high_corner, 1),
                                         at[2] + corner_offset(high_corner, 2)};

        const size_t low_index = grid.sample_index(low[0], low[1], low[2]);
        const size_t high_index = grid.sample_index(high[0], high[1], high[2]);
        const double low_value = grid.density_at(low_index);
        const double high_value = grid.density_at(high_index);

        // Доля вдоль ребра. Знаменатель нулевой означает, что оба конца лежат ровно на пороге —
        // тогда вершина ставится в середину: любое другое место было бы столь же произвольным, но
        // зависело бы от порядка вычитания.
        const double difference = high_value - low_value;
        const double t = difference != 0.0 ? std::clamp((settings.iso - low_value) / difference, 0.0, 1.0) : 0.5;

        const auto low_gradient = grid.gradient_at(low[0], low[1], low[2]);
        const auto high_gradient = grid.gradient_at(high[0], high[1], high[2]);

        std::array<double, 3> normal{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double low_coordinate = bound.position.get(low_index, axis);
          const double high_coordinate = bound.position.get(high_index, axis);
          edge_position[edge][axis] = low_coordinate + t * (high_coordinate - low_coordinate);
          // Нормаль — минус градиент: плотность растёт внутрь вещества, а поверхность смотрит наружу.
          normal[axis] = -(low_gradient[axis] + t * (high_gradient[axis] - low_gradient[axis]));
        }

        const double length =
          std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        for (uint32_t axis = 0; axis < 3; ++axis) {
          edge_normal[edge][axis] = length > 0.0 ? normal[axis] / length : 0.0;
        }
      };

      for (size_t i = 0; i < size_t(triangle_count) * 3; ++i) {
        const auto edge = uint32_t(row[i]);
        prepare_edge(edge);
        for (uint32_t axis = 0; axis < 3; ++axis) {
          bound.out_position.set(cursor, edge_position[edge][axis], axis);
          bound.out_normal.set(cursor, edge_normal[edge][axis], axis);
        }
        ++cursor;
      }
    }
  };

  if (parallel) {
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
      call.pool->submit([&emit_chunk](const size_t index) { emit_chunk(index); }, chunk);
    }
    call.pool->compute();
    call.pool->wait();
  } else {
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
      emit_chunk(chunk);
    }
  }

  bound.out_count.set(0, double(running));
}

} // namespace

void tool_registry::add_volume_tools() {
  add(tool_description{.name = "marching_cubes",
                       .shape = aperture::scatter,
                       .input_count = 2,
                       .output_count = 3,
                       .body = tool_marching_cubes});
}

} // namespace originator
} // namespace devils_engine
