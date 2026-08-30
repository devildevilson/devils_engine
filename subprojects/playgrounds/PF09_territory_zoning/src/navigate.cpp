#include "navigate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace devils_engine::pf09 {

namespace {

constexpr float arrive_epsilon = 0.05f;
constexpr float step_inside = 0.12f; // насколько переступить за проём, чтобы точно оказаться в новой зоне

glm::vec2 centre_of(const zone_store& store, const part_ref& reference) {
  const auto outline = store.outline_of(reference);
  glm::vec2 sum{};
  for (const auto& point : outline) {
    sum += point;
  }
  return outline.empty() ? glm::vec2{} : sum / float(outline.size());
}

// Загнать точку строго ВНУТРЬ фигуры. Точка ровно на общем ребре по правилу пикинга принадлежит ровно
// одной зоне, и какой именно — зависит от округления; персонаж, остановившийся на ребре, оказался бы
// в зоне, из которой следующий шаг не найдёт выхода. Поэтому после любого перемещения точка подтягивается
// к центру, пока не окажется внутри.
void settle(const zone_store& store, const part_ref& reference, glm::vec2& position) {
  const auto outline = store.outline_of(reference);
  if (outline.empty() || point_in_outline(outline, position)) return;

  const auto centre = centre_of(store, reference);
  for (uint32_t attempt = 0; attempt < 8; ++attempt) {
    position += (centre - position) * 0.4f;
    if (point_in_outline(outline, position)) return;
  }
  position = centre;
}

} // namespace

bool interior_point(const zone_store& store, const part_ref& reference, glm::vec2& out) {
  const auto outline = store.outline_of(reference);
  if (outline.size() < 3) return false;

  // Часть выпукла, поэтому её центроид всегда внутри. Запасной путь оставлен на случай авторских частей,
  // выпуклость которых сборщик не гарантирует.
  glm::vec2 middle{};
  for (const auto& point : outline) {
    middle += point;
  }
  middle /= float(outline.size());
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

std::vector<part_ref> find_path(const zone_store& store, const part_ref from, const part_ref to,
                                const uint32_t budget) {
  if (from == to) return {};
  if (store.part_of(from) == nullptr || store.part_of(to) == nullptr) return {};

  const auto* goal_zone = store.find(to.zone);
  if (goal_zone == nullptr || !store.passable(*goal_zone)) return {};

  // Поиск стал по СТОИМОСТИ, а не по числу шагов: дорога должна тянуть маршрут на себя, иначе персонаж
  // пойдёт напрямик через дворы просто потому, что там меньше клеток. Непроходимое место не пропускается
  // фильтром результата, а не рассматривается вовсе — путь сквозь стену был бы враньём, которое
  // обнаружилось бы только при движении.
  struct entry {
    float cost = 0.0f;
    part_ref where{};

    bool operator<(const entry& other) const noexcept { return cost > other.cost; }
  };

  std::map<part_ref, part_ref> came_from;
  std::map<part_ref, float> best;
  std::priority_queue<entry> queue;

  came_from[from] = from;
  best[from] = 0.0f;
  queue.push({0.0f, from});

  bool found = false;
  while (!queue.empty() && !found && best.size() < budget) {
    const auto here = queue.top();
    queue.pop();

    const auto known = best.find(here.where);
    if (known == best.end() || here.cost > known->second) continue;

    for (const auto& portal : store.portals_of(here.where)) {
      if (!portal.passable() || portal.other == invalid_key) continue;

      const part_ref next{portal.other, portal.other_part};
      // Проходимость спрашивается У ХРАНИЛИЩА, а не у записи: закрытая дверь — это рантайм-состояние
      // МЕСТА, и путь обязан меняться от того, что её заперли, без пересборки файлов.
      const auto* zone = store.find(next.zone);
      if (zone == nullptr || !store.passable(*zone)) continue;
      if (store.part_of(next) == nullptr) continue; // сосед в невыгруженном секторе

      const float step = zone->road() ? 0.5f : 1.0f;
      const float cost = here.cost + step;

      const auto seen = best.find(next);
      if (seen != best.end() && seen->second <= cost) continue;

      best[next] = cost;
      came_from[next] = here.where;
      if (next == to) {
        found = true;
        break;
      }
      queue.push({cost, next});
    }
  }

  if (!found) return {};

  std::vector<part_ref> path;
  for (auto step = to; step != from; step = came_from[step]) {
    path.push_back(step);
  }
  std::reverse(path.begin(), path.end());
  return path;
}

// Ломаная маршрута для рисования: где персонаж сейчас, через какие проёмы он пойдёт и куда придёт.
// Строится ИЗ ТОГО ЖЕ пути, по которому он шагает, а не из отдельного «визуального» маршрута: иначе
// нарисованная линия и настоящее движение разошлись бы, и картинка врала бы ровно там, где она нужна.
std::vector<glm::vec2> route_points(const zone_store& store, const agent& walker) {
  std::vector<glm::vec2> points;
  if (walker.arrived || walker.cursor >= walker.path.size()) return points;

  points.push_back(walker.position);
  auto here = walker.location;
  for (uint32_t index = walker.cursor; index < walker.path.size(); ++index) {
    const auto next = walker.path[index];
    const auto portals = store.portals_of(here);
    const auto gate = std::find_if(portals.begin(), portals.end(), [&](const zone_portal& item) {
      return item.other == next.zone && item.other_part == next.part;
    });
    // У связи без геометрии точки нет — рисовать её нечем, и подставлять центр было бы выдумкой.
    // Обрываем линию: лестница и правда не отрезок на плане.
    if (gate == portals.end() || !gate->geometric()) break;
    points.push_back(gate->middle());
    here = next;
  }

  glm::vec2 last{};
  if (here == walker.path.back() && interior_point(store, here, last)) points.push_back(last);
  return points;
}

bool step_agent(const zone_store& store, agent& walker, const float distance_m) {
  if (walker.arrived || walker.cursor >= walker.path.size()) {
    walker.arrived = true;
    return false;
  }

  if (store.part_of(walker.location) == nullptr) return false;

  const auto next = walker.path[walker.cursor];
  const auto portals = store.portals_of(walker.location);
  const auto gate = std::find_if(portals.begin(), portals.end(), [&](const zone_portal& item) {
    return item.other == next.zone && item.other_part == next.part && item.passable();
  });
  if (gate == portals.end()) return false;
  if (store.part_of(next) == nullptr) return false;

  // У связи без геометрии (дорога между поселениями, ребро политического графа) отрезка нет. Такой
  // переход мгновенный: он и означает не «пройти», а «переместиться» — и притворяться, что персонаж
  // шагает по нему метр за метром, было бы ложью про масштаб.
  const bool abstract_gate = !gate->geometric();
  if (abstract_gate) {
    walker.location = next;
    ++walker.cursor;
    if (!interior_point(store, next, walker.position)) walker.position = centre_of(store, next);
    settle(store, next, walker.position);
    walker.arrived = walker.cursor >= walker.path.size();
    return true;
  }

  const auto aim = gate->middle();
  const auto delta = aim - walker.position;
  const float remaining = std::sqrt(delta.x * delta.x + delta.y * delta.y);

  if (remaining > distance_m + arrive_epsilon) {
    const auto stepped = walker.position + delta * (distance_m / remaining);

    // Если шаг вывел из своей части — значит проём уже достигнут, и надо переступать, а не возвращаться.
    // Прежняя редакция подтягивала точку обратно к центру, шаг снова выводил её наружу, и персонаж
    // колебался на месте бесконечно: за двести тысяч шагов он так и не проходил один проём.
    const auto outline = store.outline_of(walker.location);
    if (outline.empty() || point_in_outline(outline, stepped)) {
      walker.position = stepped;
      return true;
    }
  }

  // Переступаем проём: становимся чуть ЗА ним, в сторону следующей зоны. Останавливаться ровно на ребре
  // нельзя — точка на общей границе принадлежит ровно одной зоне по правилу пикинга, и какой именно,
  // зависело бы от округления.
  const auto beyond = centre_of(store, next);
  const auto inward = beyond - aim;
  const float length = std::sqrt(inward.x * inward.x + inward.y * inward.y);

  walker.position = length > 1.0e-4f ? aim + inward * (step_inside / length) : beyond;

  // Проём может быть длинным и узким, а центр соседней зоны — почти напротив него: тогда шага внутрь не
  // хватает, чтобы точка гарантированно оказалась в новой фигуре. Дотягиваем к центру, пока не окажемся
  // внутри. Оставить точку снаружи нельзя: следующий шаг искал бы проход из зоны, в которой персонажа нет.
  settle(store, next, walker.position);

  walker.location = next;
  ++walker.cursor;
  walker.arrived = walker.cursor >= walker.path.size();
  return true;
}

} // namespace devils_engine::pf09
