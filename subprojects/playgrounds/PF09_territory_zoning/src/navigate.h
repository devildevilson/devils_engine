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

#include <glm/vec2.hpp>

#include "zones.h"

namespace devils_engine::pf09 {

struct agent {
  zone_key zone = invalid_key;
  glm::vec2 position{};
  std::vector<zone_key> path;   // оставшиеся зоны, начиная со следующей
  uint32_t cursor = 0;
  bool arrived = false;
};

// Поиск в ширину по проходимым порталам. Бюджет узлов ограничен намеренно: путь через полгорода игре не
// нужен, а неограниченный обход на подгруженном мире означал бы обход всего резидентного.
std::vector<zone_key> find_path(const zone_store& store, const zone_key from, const zone_key to,
                                const uint32_t budget = 65536);

// Один шаг. Персонаж идёт к проёму, ведущему в следующую зону; дойдя, переступает в неё. Возвращает
// false, если идти больше некуда.
bool step_agent(const zone_store& store, agent& walker, const float distance_m);

// Точка внутри зоны, от которой удобно стартовать: центр габарита, если он внутри фигуры, иначе первая
// подходящая точка вдоль диагонали.
bool interior_point(const zone_store& store, const zone_record& record, glm::vec2& out);

} // namespace devils_engine::pf09

#endif
