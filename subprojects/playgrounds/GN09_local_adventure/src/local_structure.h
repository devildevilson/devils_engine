#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "adventure.h"

namespace gn09 {

// План искусственного места живёт в сантиметрах, отдельно от сэмплов пещеры.
// Оси u/v лежат в X/Z; u выбирается по свободному направлению от портов.
struct plan_rect {
  int32_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

struct built_zone {
  plan_rect box;
  std::string purpose;
};

struct built_opening {
  uint32_t a = 0, b = 0;
  // Отрезок на общей стене зон: либо u0==u1, либо v0==v1.
  plan_rect span;
};

struct built_port {
  uint32_t edge_index = 0;
  plan_rect span; // проём дока на его внешней стороне
};

enum class hatch_condition : uint8_t { operable, jammed, breached };
const char* name(hatch_condition condition);

struct emergency_exit {
  uint32_t zone = 0; // модуль, из которого дверь ведёт в пещеру
  plan_rect span;     // точный отрезок на наружной стене модуля
  hatch_condition condition = hatch_condition::operable;
};

// Руда, кристалл или кладка: локальный объект, не отдельный узел маршрута.
struct local_feature {
  uint32_t room = 0;
  point centre;
  std::string kind;
  uint32_t amount = 0;
};

struct local_structure {
  uint32_t node_id = 0;
  theme kind = theme::empty;
  site_recipe recipe = site_recipe::ore_cave;
  point centre;
  point forward{1, 0, 0};
  int8_t dock_axis = 0; // 0: боковой шлюз; +1: база над лодкой; -1: под ней
  int32_t level_cm = 0; // высота построенных зон относительно графового узла
  hatch_condition dock_hatch = hatch_condition::operable;
  std::vector<built_zone> zones;
  std::vector<built_opening> openings;
  std::vector<built_port> ports;
  std::vector<emergency_exit> emergency_exits;
  std::vector<local_feature> features;
};

// Цельные детали постройки в локальных сантиметрах. Стены — отдельный материал,
// поэтому вырезание скалы не может незаметно удалить перегородку.
struct built_solid {
  plan_rect footprint;
  int32_t y0 = 0, y1 = 0;
  uint8_t shade = 0;
  bool hatch_panel = false;
};

local_structure materialize_local_structure(const adventure& world, uint32_t node_id);
int8_t preferred_dock_axis(uint64_t seed, const node& at);
// Проверяет прямоугольники, проёмы и привязку залежей независимо от генератора плана.
std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure);
point structure_to_world(const local_structure& structure, double u, double y, double v);
point docking_axis_point(const local_structure& structure, double progress, double along);
double outpost_envelope_field(const local_structure& structure, point position);
// Проверяет природный док до монтажа и долю корпуса, помещённого в породу.
std::string verify_outpost_environment(const adventure& world,
                                       const local_structure& structure);
uint32_t outpost_body_rock_percent(const adventure& world,
                                   const local_structure& structure);
std::vector<built_solid> materialize_outpost_solids(const local_structure& structure);
std::vector<built_solid> fit_outpost_solids(const adventure& world,
                                           const local_structure& structure);
// Проверяет уже построенные твёрдые детали относительно резервов графа.
std::string verify_outpost_solids(const adventure& world, const local_structure& structure,
                                  const std::vector<built_solid>& solids);
bool write_structure_svg(const adventure& world, uint32_t node_id, const std::string& path);

} // namespace gn09
