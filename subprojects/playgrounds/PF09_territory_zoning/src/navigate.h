#ifndef DEVILS_ENGINE_PF09_NAVIGATE_H
#define DEVILS_ENGINE_PF09_NAVIGATE_H

// Движение по зонам через порталы. Это и есть проверка того, что зональность пригодна для игры: если по
// ней нельзя провести персонажа, значит она описывает картинку, а не мир.
//
// Портал хранит сам отрезок общего ребра, поэтому маршрут получается не последовательностью центров зон,
// а последовательностью ПРОЁМОВ — как в navmesh. Персонаж целится в проём, а не в середину комнаты, и
// поэтому не режет углы сквозь стены.

#include <cstdint>
#include <vector>

#include <glm/common.hpp>
#include <glm/vec2.hpp>

#include "zones.h"

namespace devils_engine::pf09 {

// Габарит актора. Всё, что связано с обходом предметов, меряется им: предмет мешает не точке, а телу.
constexpr float agent_radius_m = 0.35f;

struct agent {
  part_ref location{};          // ЧАСТЬ, а не зона: идти можно только по выпуклому
  glm::vec2 position{};
  std::vector<part_ref> path;   // оставшиеся части, начиная со следующей
  uint32_t cursor = 0;
  bool arrived = false;
};

// Поиск в ширину по проходимым порталам. Бюджет узлов ограничен намеренно: путь через полгорода игре не
// нужен, а неограниченный обход на подгруженном мире означал бы обход всего резидентного.
std::vector<part_ref> find_path(const zone_store& store, const part_ref from, const part_ref to,
                                const uint32_t budget = 65536);

// Один шаг. Персонаж идёт к проёму, ведущему в следующую зону; дойдя, переступает в неё. Возвращает
// false, если идти больше некуда.
bool step_agent(const zone_store& store, agent& walker, const float distance_m);

// Точка занята предметом, сквозь который не пройти. Нужна и шагу, и расстановке: персонаж, заведённый
// внутри стола, из него уже не выйдет обычным шагом.
bool blocked_by_prop(const zone_store& store, const part_ref& reference, const glm::vec2 point);

// Ломаная маршрута для рисования: позиция персонажа, середины проёмов на пути и точка прибытия.
std::vector<glm::vec2> route_points(const zone_store& store, const agent& walker);

// Точка внутри зоны, от которой удобно стартовать: центр габарита, если он внутри фигуры, иначе первая
// подходящая точка вдоль диагонали.
bool interior_point(const zone_store& store, const part_ref& reference, glm::vec2& out);

} // namespace devils_engine::pf09

#endif
