#ifndef GN07_FRAME_H
#define GN07_FRAME_H

#include <cstdint>
#include <string>
#include <vector>

#include "location_model.h"

// Каркас: граф, воплощённый в план участка. Узел графа становится ЗОНОЙ — несколькими
// прямоугольниками в сантиметрах внутри своей клетки решётки; ребро графа — ПРОЁМОМ на общей
// стороне двух клеток. Растра нет: план живёт в единицах, картинка — вид (SVG).
//
// Почему план собирается по клеткам. Узлы графа сторонами клеток касаются только своих соседей
// по графу (так семейство растит дерево), поэтому всё, что зона кладёт внутри клетки с полем в
// полстены, отделено от несвязанных зон толщей не тоньше стены. До границы клетки доходят только
// устья — полуходы к соседу по графу, и на этой границе устья двух зон встречаются проёмом.
// Значит, связи плана совпадают со связями графа ПО ПОСТРОЕНИЮ — а проверка всё равно выводит их
// заново из прямоугольников и сравнивает с графом, потому что построение могло ошибиться.
//
// Вид выглядит наращённым, а не начерченным: коридор на каждом шаге решётки уходит вбок не
// больше чем на `wander`, сечения и камеры разной величины, у забоев и лагеря есть наросты.
//
// Роли частей нужны только самопроверкам (какую часть портить) и виду; проверка их не читает.

namespace gn07 {

struct plan_rect {
  int32_t x0 = 0;               // см; x на восток, y на юг, начало — северо-западный угол участка
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
};

enum class part_role : uint8_t { core, lobe, mouth, portal };

struct frame_part {
  plan_rect box;
  part_role role = part_role::core;
  uint32_t toward = no_node;    // у устья — сосед по графу, к которому оно ведёт
};

struct frame_zone {
  uint32_t node = no_node;
  int32_t anchor_x = 0;         // точка, через которую идёт маршрут по зоне
  int32_t anchor_y = 0;
  std::vector<frame_part> parts;
};

struct location_frame {
  int32_t width = 0;            // см
  int32_t height = 0;
  std::vector<frame_zone> zones; // зона i — узел i графа
};

struct frame_result {
  location_frame frame;
  std::string refusal;
  bool valid() const noexcept { return refusal.empty(); }
};

frame_result build_frame(const location_graph& graph, const archetype& arch, uint64_t seed);

// ---- Независимый разбор плана --------------------------------------------------------------
//
// Всё ниже выводится из одних прямоугольников: ни граф, ни роли частей не читаются.

struct frame_join {
  uint32_t a = 0;               // a < b
  uint32_t b = 0;
  int32_t width = 0;            // самый длинный сплошной отрезок касания
  bool vertical = false;        // касание по вертикальной линии x = line
  int32_t line = 0;
  int32_t mid = 0;              // середина этого отрезка вдоль линии
};

struct frame_opening {
  uint32_t zone = 0;
  side at = side::none;
  int32_t from = 0;             // см вдоль стороны: x для north/south, y для west/east
  int32_t to = 0;
};

struct frame_gap {
  uint32_t a = 0;               // a < b, зоны не касаются
  uint32_t b = 0;
  int64_t distance_sq = 0;      // см², наименьшее расстояние между их частями
};

struct frame_analysis {
  std::vector<frame_join> joins;           // по (a, b)
  std::vector<frame_opening> openings;     // по (zone, side, from)
  std::vector<std::string> problems;       // выход за участок, наложение зон, зона из кусков
  std::vector<frame_gap> gaps;             // несвязанные пары, у которых части хоть где-то рядом
  int64_t thinnest_wall_sq = -1;           // квадрат наименьшего расстояния между несвязанными зонами
  uint32_t wall_a = no_node;
  uint32_t wall_b = no_node;
  std::vector<int64_t> zone_area;          // см², объединение частей
  size_t parts = 0;
};

frame_analysis analyse_frame(const location_frame& frame);

// Проверка каркаса против графа и входа локации. Пустой список — всё верно.
std::vector<std::string> verify_frame(const location_frame& frame, const location_graph& graph, const archetype& arch,
                                      const location_input& input);

// Длина пешего маршрута в см от проёма обычного входа до точки каждой зоны: через точки зон и
// середины проёмов, по осям. Только для сведения — длину ничто не ограничивает.
std::vector<int64_t> frame_walk(const location_frame& frame, const frame_analysis& analysis,
                                const location_graph& graph);

struct frame_mark {
  uint32_t node = 0;
  std::string label;
};

// Вид плана. Полностью определяется каркасом, поэтому его тоже сравнивает воспроизводимость.
std::string format_svg(const location_frame& frame, const location_graph& graph, const archetype& arch,
                       const std::vector<frame_mark>& marks);

// Сантиметры как метры с двумя знаками, без double.
std::string metres(int64_t cm);

} // namespace gn07

#endif
