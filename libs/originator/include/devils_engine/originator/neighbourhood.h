#ifndef DEVILS_ENGINE_ORIGINATOR_NEIGHBOURHOOD_H
#define DEVILS_ENGINE_ORIGINATOR_NEIGHBOURHOOD_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

#include "buffer.h"
#include "tools.h"

// СОСЕДСТВО КАК ПРИВЯЗКА. Инструменты, которым нужны соседи элемента (размытие, заливка, граница),
// пишутся ОДИН раз против этого аксессора, а не дважды — «для растра» и «для графа». Пары box_blur /
// graph_blur и collapse / graph_collapse появились именно потому, что адресация была зашита в
// инструмент; здесь она — данные вызова.
//
// Две формы записи одного понятия:
//   CSR       явная таблица: смещения и цели (+ необязательные веса дуг) — обычные буферы; так
//             записываются граф поверхности, скелет пещеры (из списка рёбер — `edge_csr`), что угодно;
//   трафарет  неявная таблица над формой растра: крест (4/6 соседей) или квадрат радиуса r; её не
//             разворачивают в CSR — миллион клеток по 8 соседей это 32 МБ знания, выводимого из
//             координат.
// Какая из форм стоит за вызовом, решается ОДИН раз на вызов; горячий путь — шаблон над ней.
//
// Порядок обхода фиксирован и у CSR (порядок дуг), и у трафарета (по осям, от меньшего к большему):
// результат инструментов, которые от него зависят (сумма с плавающей точкой), не зависит от того, кто
// и сколько раз спросил.

namespace devils_engine {
namespace originator {

struct csr_neighbourhood {
  const_field_accessor offsets;
  const_field_accessor arcs;
  // Вес дуги, параллельно arcs. Пусто — у всех дуг вес 1.
  const_field_accessor weights;
  size_t count = 0;

  bool weighted() const noexcept { return weights.valid(); }

  // fn(сосед, вес дуги). Дуга за пределы элементов пропускается; смещение за конец дуг не читается.
  template <typename fn_t>
  void each(const size_t cell, fn_t&& fn) const {
    const auto first = size_t(offsets.get(cell));
    const auto last = std::min(size_t(offsets.get(cell + 1)), arcs.count());
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other >= count) continue;
      fn(other, weights.valid() ? weights.get(k) : 1.0);
    }
  }
};

struct stencil_neighbourhood {
  enum class shape { cross, box };

  buffer_extent extent;
  shape form = shape::cross;
  size_t radius = 1;
  bool wrap = false;

  bool weighted() const noexcept { return false; }

  // Сосед за краем растра пропускается (или заворачивается при wrap). Себя трафарет не называет.
  template <typename fn_t>
  void each(const size_t cell, fn_t&& fn) const {
    const size_t sx = extent.x, sy = extent.y == 0 ? 1 : extent.y, sz = extent.z == 0 ? 1 : extent.z;
    const auto x = int64_t(cell % sx), y = int64_t((cell / sx) % sy), z = int64_t(cell / (sx * sy));
    const auto r = int64_t(radius);
    const auto wrap_axis = [&](int64_t v, const size_t side, bool& inside) {
      if (v >= 0 && v < int64_t(side)) return v;
      if (!wrap) {
        inside = false;
        return v;
      }
      return ((v % int64_t(side)) + int64_t(side)) % int64_t(side);
    };
    const int64_t rz = sz > 1 ? r : 0, ry = sy > 1 ? r : 0;
    for (int64_t dz = -rz; dz <= rz; ++dz) {
      for (int64_t dy = -ry; dy <= ry; ++dy) {
        for (int64_t dx = -r; dx <= r; ++dx) {
          if (dx == 0 && dy == 0 && dz == 0) continue;
          if (form == shape::cross && int(dx != 0) + int(dy != 0) + int(dz != 0) != 1) continue;
          bool inside = true;
          const int64_t nx = wrap_axis(x + dx, sx, inside), ny = wrap_axis(y + dy, sy, inside),
                        nz = wrap_axis(z + dz, sz, inside);
          if (!inside) continue;
          fn(size_t(nx) + sx * (size_t(ny) + sy * size_t(nz)), 1.0);
        }
      }
    }
  }
};

using neighbourhood = std::variant<csr_neighbourhood, stencil_neighbourhood>;

// Соседство вызова: CSR, если привязаны входы `offsets_input` и следующий за ним (цели), а при
// `weights` ещё и третий — веса дуг; иначе трафарет над формой привязки `shaped` по параметрам
// `stencil` (cross | box), `radius`, `wrap`. Растр без объявленной формы — громкий отказ: соседей у
// линейного буфера нет.
neighbourhood resolve_neighbourhood(const tool_call& call, const size_t offsets_input, const field_ref& shaped,
                                    const size_t count);

// ЯДРА — одно на понятие, над любой формой соседства. Старые инструменты (`graph_blur`, `box_blur`,
// `graph_flood`, `graph_frontier`, `graph_slope`, `graph_vote`) зовут ровно их, поэтому их результат не изменился ни в одном бите.

// Взвешенное среднее себя и соседей: self_weight * v[i] + сумма (neighbour_weight * вес дуги * v[j]),
// делённое на сумму весов. Квадратный трафарет 2D с равными весами идёт путём box_mean — так же, как
// box_blur, построчно по окну вместе с собой, и это тот же результат бит в бит.
void blur_range(const neighbourhood& around, const const_field_accessor& values, const field_accessor& target,
                const size_t begin, const size_t end, const double self_weight, const double neighbour_weight);
// Среднее по окну (2r+1)^2 растра шириной width, обрезанному по краям; построчно, вместе с собой.
void box_mean_range(const const_field_accessor& source, const field_accessor& target, const size_t width,
                    const size_t height, const size_t radius, const size_t begin, const size_t end);
// 1, если у элемента есть сосед с другой меткой (метка `ignored` границей не считается).
void frontier_range(const neighbourhood& around, const const_field_accessor& labels, const field_accessor& target,
                    const size_t begin, const size_t end, const double ignored);
// Средний модуль разности со соседями (вес дуги не учитывается); без соседей — 0.
void slope_range(const neighbourhood& around, const const_field_accessor& values, const field_accessor& target,
                 const size_t begin, const size_t end);
// Голосование: элемент без метки (0) под маской берёт метку соседа с наибольшим ВЕСОМ ЭЛЕМЕНТА-соседа,
// строго выше threshold; равные веса — меньшая метка, чтобы ответ не зависел от порядка дуг. Элемент с
// меткой или вне маски сохраняет свою.
void vote_range(const neighbourhood& around, const const_field_accessor& labels, const const_field_accessor& weights,
                const const_field_accessor& mask, const field_accessor& target, const size_t begin, const size_t end,
                const double threshold);
// Заливка от затравок (метка > 0) по проходимым элементам: шаг в соседа стоит cost[сосед] плюс вес
// дуги, если соседство взвешено. Очередь упорядочена по (расстояние, метка, элемент), capacity > 0 —
// предел элементов на метку. Пишет метку (0 — не достигнут) и расстояние (unreached).
void flood_whole(const neighbourhood& around, const const_field_accessor& seeds, const const_field_accessor& costs,
                 const const_field_accessor& passable, const field_accessor& label_out,
                 const field_accessor& distance_out, const size_t count, const double unreached,
                 const size_t capacity, const std::string_view& step_name);

} // namespace originator
} // namespace devils_engine

#endif
