#include "navigate.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace devils_engine::pf09 {

namespace {

constexpr float arrive_epsilon = 0.05f;
constexpr float step_inside = 0.12f; // насколько переступить за проём, чтобы точно оказаться в новой зоне

glm::vec2 centre_of(const zone_store& store, const zone_record& record) {
  const auto outline = store.outline_of(record);
  glm::vec2 sum{};
  for (const auto& point : outline) {
    sum += point;
  }
  return outline.empty() ? glm::vec2{record.bounds.lower.x, record.bounds.lower.z} : sum / float(outline.size());
}

// Загнать точку строго ВНУТРЬ фигуры. Точка ровно на общем ребре по правилу пикинга принадлежит ровно
// одной зоне, и какой именно — зависит от округления; персонаж, остановившийся на ребре, оказался бы
// в зоне, из которой следующий шаг не найдёт выхода. Поэтому после любого перемещения точка подтягивается
// к центру, пока не окажется внутри.
void settle(const zone_store& store, const zone_record& record, glm::vec2& position) {
  const auto outline = store.outline_of(record);
  if (outline.empty() || point_in_outline(outline, position)) return;

  const auto centre = centre_of(store, record);
  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    position += (centre - position) * 0.4f;
    if (point_in_outline(outline, position)) return;
  }
  position = centre;
}

} // namespace

bool interior_point(const zone_store& store, const zone_record& record, glm::vec2& out) {
  const auto outline = store.outline_of(record);
  if (outline.size() < 3) return false;

  const glm::vec2 middle{(record.bounds.lower.x + record.bounds.upper.x) * 0.5f,
                         (record.bounds.lower.z + record.bounds.upper.z) * 0.5f};
  if (point_in_outline(outline, middle)) {
    out = middle;
    return true;
  }

  // Невыпуклая фигура может не содержать центр габарита. Тогда пробуем точки, смещённые к вершинам:
  // рядом с вершиной внутренность есть всегда, если фигура невырождена.
  for (const auto& vertex : outline) {
    const glm::vec2 probe = vertex + (middle - vertex) * 0.05f;
    if (point_in_outline(outline, probe)) {
      out = probe;
      return true;
    }
  }
  return false;
}

std::vector<zone_key> find_path(const zone_store& store, const zone_key from, const zone_key to,
                                const uint32_t budget) {
  if (from == to) return {};
  if (store.find(from) == nullptr || store.find(to) == nullptr) return {};

  std::map<zone_key, zone_key> came_from;
  std::vector<zone_key> queue{from};
  came_from[from] = from;

  // Бюджет ограничивает РАСШИРЕНИЕ очереди, а не сам обход. Прежняя редакция обрывала цикл при
  // достижении бюджета целиком, и путь длиной в город объявлялся несуществующим ровно тогда, когда город
  // оказывался чуть больше бюджета — то есть проверка падала не на ошибке, а на размере.
  bool found = false;
  for (size_t head = 0; head < queue.size() && !found; ++head) {
    const auto* record = store.find(queue[head]);
    if (record == nullptr) continue;

    for (const auto& portal : store.portals_of(*record)) {
      // Запертый проход не участвует в поиске. Это не фильтр результата, а именно правило обхода: путь,
      // построенный через замок, был бы враньём, которое обнаружилось бы только при движении.
      if (!portal.passable() || portal.other == invalid_key) continue;
      if (came_from.contains(portal.other)) continue;
      if (store.find(portal.other) == nullptr) continue; // сосед в невыгруженном секторе

      came_from[portal.other] = queue[head];
      if (portal.other == to) {
        found = true;
        break;
      }
      if (queue.size() < budget) queue.push_back(portal.other);
    }
  }

  if (!found) return {};

  std::vector<zone_key> path;
  for (auto step = to; step != from; step = came_from[step]) {
    path.push_back(step);
  }
  std::reverse(path.begin(), path.end());
  return path;
}

bool step_agent(const zone_store& store, agent& walker, const float distance_m) {
  if (walker.arrived || walker.cursor >= walker.path.size()) {
    walker.arrived = true;
    return false;
  }

  const auto* here = store.find(walker.zone);
  if (here == nullptr) return false;

  const auto next = walker.path[walker.cursor];
  const auto portals = store.portals_of(*here);
  const auto gate = std::find_if(portals.begin(), portals.end(), [&](const zone_portal& item) {
    return item.other == next && item.passable();
  });
  if (gate == portals.end()) return false;

  const auto* target = store.find(next);
  if (target == nullptr) return false;

  // У связи без геометрии (дорога между поселениями, ребро политического графа) отрезка нет. Такой
  // переход мгновенный: он и означает не «пройти», а «переместиться» — и притворяться, что персонаж
  // шагает по нему метр за метром, было бы ложью про масштаб.
  const bool abstract_gate = !gate->geometric();
  if (abstract_gate) {
    walker.zone = next;
    ++walker.cursor;
    if (!interior_point(store, *target, walker.position)) walker.position = centre_of(store, *target);
    settle(store, *target, walker.position);
    walker.arrived = walker.cursor >= walker.path.size();
    return true;
  }

  const auto aim = gate->middle();
  const auto delta = aim - walker.position;
  const float remaining = std::sqrt(delta.x * delta.x + delta.y * delta.y);

  if (remaining > distance_m + arrive_epsilon) {
    walker.position += delta * (distance_m / remaining);
    settle(store, *here, walker.position);
    return true;
  }

  // Переступаем проём: становимся чуть ЗА ним, в сторону следующей зоны. Останавливаться ровно на ребре
  // нельзя — точка на общей границе принадлежит ровно одной зоне по правилу пикинга, и какой именно,
  // зависело бы от округления.
  const auto beyond = centre_of(store, *target);
  const auto inward = beyond - aim;
  const float length = std::sqrt(inward.x * inward.x + inward.y * inward.y);

  walker.position = length > 1.0e-4f ? aim + inward * (step_inside / length) : beyond;

  // Проём может быть длинным и узким, а центр соседней зоны — почти напротив него: тогда шага внутрь не
  // хватает, чтобы точка гарантированно оказалась в новой фигуре. Дотягиваем к центру, пока не окажемся
  // внутри. Оставить точку снаружи нельзя: следующий шаг искал бы проход из зоны, в которой персонажа нет.
  settle(store, *target, walker.position);

  walker.zone = next;
  ++walker.cursor;
  walker.arrived = walker.cursor >= walker.path.size();
  return true;
}

} // namespace devils_engine::pf09
