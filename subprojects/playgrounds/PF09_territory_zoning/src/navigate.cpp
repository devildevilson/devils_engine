#include "navigate.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace devils_engine::pf09 {

namespace {

constexpr float arrive_epsilon = 0.05f;
constexpr float step_inside = 0.12f; // насколько переступить за проём, чтобы точно оказаться в новой зоне

// Ровно на границе зазора стоять нельзя, и это не придирка. Поворот направления ДО КАСАТЕЛЬНОЙ по
// построению даёт ближайшее сближение, равное зазору ТОЧНО; а «точно» — состояние, которого две разные
// формулы (одна считает точку из угла, другая расстояние из точки) не удержат одинаково. Персонаж
// оказывался внутри предмета на четыре десятитысячных, ровно семь раз из сорока шести тысяч шагов.
// Поэтому и касательная берётся с запасом, и выталкивание срабатывает по запасу, а не по самому зазору.
constexpr float push_margin = 1.01f;

// Свободная точка внутри части: середина, если она не занята мебелью, иначе поиск вдоль лучей к вершинам.
// Объявлена заранее, потому что ею же чинится и подтяжка после перехода.
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
  if (outline.empty()) return;

  if (!point_in_outline(outline, position)) {
    const auto centre = centre_of(store, reference);
    bool inside = false;
    for (uint32_t attempt = 0; attempt < 8 && !inside; ++attempt) {
      position += (centre - position) * 0.4f;
      inside = point_in_outline(outline, position);
    }
    if (!inside) position = centre;
  }

  // Внутри фигуры — ещё не значит на свободном месте. Переступив проём или подтянувшись к центру, точка
  // может оказаться в мебели, и обычным шагом персонаж оттуда уже не выйдет: шаг обходит предмет, стоящий
  // ВПЕРЕДИ, а не тот, внутри которого он стоит. Выталкиваем сразу.
  const auto props = store.props_of(reference.zone);
  for (uint32_t pass = 0; pass < 4; ++pass) {
    bool moved = false;
    for (const auto& prop : props) {
      if (prop.part != reference.part || !prop.blocks_move()) continue;
      const float clearance = prop.radius + agent_radius_m;
      auto delta = position - prop.position;
      const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
      if (length >= clearance * push_margin) continue;
      delta = length > 1.0e-4f ? delta / length : glm::vec2{1.0f, 0.0f};
      position = prop.position + delta * (clearance * push_margin);
      moved = true;
    }
    if (!moved) break;
  }
  // Последняя попытка. Раньше здесь стоял центр габарита, и он же был последней дырой: центр комнаты
  // прекрасно бывает занят столом, и персонаж оказывался внутри него — редко, семь шагов из сорока шести
  // тысяч, но ровно так и выглядят баги, которые ловятся раз в час игры.
  if (!point_in_outline(outline, position) || blocked_by_prop(store, reference, position)) {
    glm::vec2 free_spot{};
    position = interior_point(store, reference, free_spot) ? free_spot : centre_of(store, reference);
  }
}

// Обход предмета. Толкать точку наружу мало: предмет ровно между персонажем и проёмом выталкивает его
// обратно на ту же прямую, и получается либо орбита, либо топтание. Поэтому направление ПОВОРАЧИВАЕТСЯ
// до касательной к окружности предмета — ровно настолько, чтобы разойтись, и в ту сторону, которая ближе
// к исходной цели. Это местная задача шага: маршрут о мебели не знает и знать не должен, связность живёт
// на частях, а часть предмет не разрезает.
glm::vec2 steer_around(const std::span<const zone_prop> props, const uint32_t part, const glm::vec2 from,
                       const glm::vec2 aim, const float distance_m) {
  auto direction = aim - from;
  const float reach = std::sqrt(direction.x * direction.x + direction.y * direction.y);
  if (reach < 1.0e-4f) return from;
  direction /= reach;

  const zone_prop* nearest = nullptr;
  float nearest_along = 1.0e30f;
  for (const auto& prop : props) {
    if (prop.part != part || !prop.blocks_move()) continue;

    const auto to_centre = prop.position - from;
    const float along = to_centre.x * direction.x + to_centre.y * direction.y;
    const float clearance = prop.radius + agent_radius_m;
    if (along <= 0.0f || along > distance_m + clearance) continue;

    const float side = std::abs(to_centre.x * direction.y - to_centre.y * direction.x);
    if (side >= clearance) continue;
    if (along < nearest_along) {
      nearest_along = along;
      nearest = &prop;
    }
  }

  if (nearest != nullptr) {
    const auto to_centre = nearest->position - from;
    const float centre_distance = std::sqrt(to_centre.x * to_centre.x + to_centre.y * to_centre.y);
    const float clearance = nearest->radius + agent_radius_m;

    // Внутри предмета касательной нет — там остаётся только выталкивание, и оно ниже.
    if (centre_distance > clearance) {
      const float half_angle = std::asin(std::clamp(clearance * push_margin / centre_distance, 0.0f, 1.0f));
      const float to_centre_angle = std::atan2(to_centre.y, to_centre.x);
      const float direction_angle = std::atan2(direction.y, direction.x);
      float offset = direction_angle - to_centre_angle;
      while (offset > 3.14159265f) offset -= 6.28318531f;
      while (offset < -3.14159265f) offset += 6.28318531f;

      const float turned = to_centre_angle + (offset >= 0.0f ? half_angle : -half_angle);
      direction = {std::cos(turned), std::sin(turned)};
    }
  }

  auto stepped = from + direction * distance_m;

  // Страховка: если после поворота точка всё равно оказалась в предмете (например, шаг начался вплотную),
  // выталкиваем её наружу. Без этого персонаж однажды окажется внутри стола, и уже не выйдет.
  for (const auto& prop : props) {
    if (prop.part != part || !prop.blocks_move()) continue;
    const float clearance = prop.radius + agent_radius_m;
    auto delta = stepped - prop.position;
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (length >= clearance * push_margin) continue;
    delta = length > 1.0e-4f ? delta / length : glm::vec2{1.0f, 0.0f};
    stepped = prop.position + delta * (clearance * push_margin);
  }
  return stepped;
}

} // namespace

bool blocked_by_prop(const zone_store& store, const part_ref& reference, const glm::vec2 point) {
  for (const auto& prop : store.props_of(reference.zone)) {
    if (prop.part != reference.part || !prop.blocks_move()) continue;
    const auto delta = point - prop.position;
    if (delta.x * delta.x + delta.y * delta.y < (prop.radius + agent_radius_m) * (prop.radius + agent_radius_m)) {
      return true;
    }
  }
  return false;
}

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
  if (point_in_outline(outline, middle) && !blocked_by_prop(store, reference, middle)) {
    out = middle;
    return true;
  }

  // Центр занят мебелью или фигура невыпукла — ищем свободное место вдоль лучей к вершинам, а затем по
  // сетке внутри габарита. Сетка нужна не для красоты: без неё оставались части, где свободная точка есть,
  // но ни на одном из лучей не лежит, и персонаж оказывался внутри стола — семь шагов из сорока шести
  // тысяч. Ровно такие баги и ловятся раз в час игры, а не проверкой.
  for (const auto& vertex : outline) {
    for (const float t : {0.05f, 0.25f, 0.45f, 0.70f}) {
      const glm::vec2 probe = vertex + (middle - vertex) * t;
      if (point_in_outline(outline, probe) && !blocked_by_prop(store, reference, probe)) {
        out = probe;
        return true;
      }
    }
  }

  glm::vec2 lower = outline[0];
  glm::vec2 upper = outline[0];
  for (const auto& point : outline) {
    lower = glm::min(lower, point);
    upper = glm::max(upper, point);
  }
  constexpr uint32_t side = 9;
  for (uint32_t i = 0; i < side * side; ++i) {
    const glm::vec2 probe{lower.x + (upper.x - lower.x) * (float(i % side) + 0.5f) / float(side),
                          lower.y + (upper.y - lower.y) * (float(i / side) + 0.5f) / float(side)};
    if (point_in_outline(outline, probe) && !blocked_by_prop(store, reference, probe)) {
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
    const auto stepped = steer_around(store.props_of(walker.location.zone), walker.location.part,
                                      walker.position, aim, distance_m);

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
