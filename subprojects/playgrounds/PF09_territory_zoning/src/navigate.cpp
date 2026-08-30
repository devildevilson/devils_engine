#include "navigate.h"

#include "devils_engine/acumen/astar.h"

#include "titles.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace devils_engine::pf09 {

// Замер последнего поиска. Отдельная переменная здесь честнее параметра: число нужно только замерам, и
// протаскивать его через сигнатуру ради инструмента значило бы менять контракт под инструмент.
namespace {
size_t g_last_search_nodes = 0;
}

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

size_t last_search_nodes() noexcept { return g_last_search_nodes; }

route_exposure measure_route(const zone_store& store, const std::span<const part_ref> path,
                             const uint32_t hostile_faction) {
  route_exposure out{};
  zone_key last = invalid_key;
  for (const auto& step : path) {
    if (step.zone == last) continue; // одна зона из нескольких частей считается один раз
    last = step.zone;
    ++out.steps;
    out.crime_sum += store.control_at(step.zone, control_field::crime).crime;
    if (hostile_faction != 0 &&
        store.control_at(step.zone, control_field::faction).faction == hostile_faction) {
      ++out.hostile_steps;
    }
  }
  return out;
}

// Поиск пути отдан ОБЩЕМУ A* движка (`devils_engine::astar`). Своя реализация здесь была лишней: чинить
// и ускорять поиск по графу надо в одном месте, а не в каждой площадке заново.
//
// Эвристика НУЛЕВАЯ, и это не лень. Стоимость шага здесь — не расстояние, а `1/скорость`, умноженная на
// штрафы за преступность, чужую территорию и чужую фракцию; связать её с метрами нечем, а завышенная
// оценка снизу сделала бы A* просто НЕПРАВИЛЬНЫМ — он вернул бы не кратчайший путь и молча. С нулевой
// эвристикой A* вырождается в алгоритм Дейкстры, что для этой задачи и есть верный ответ.
struct path_query {
  const zone_store* store = nullptr;
  const travel_policy* policy = nullptr;
  std::map<zone_key, zone_control>* control_cache = nullptr;
};

class path_rules final : public astar<part_ref>::interface {
public:
  astar<part_ref>::float_t neighbor_cost(const part_ref& from, const part_ref& to,
                                         const void* payload) const override {
    const auto* query = static_cast<const path_query*>(payload);
    const auto& policy = *query->policy;
    const auto* zone = query->store->find(to.zone);
    if (zone == nullptr) return 1.0e9;
    (void)from;

    float step = zone->step_cost();

    // Территория дорожает шаг. Подъём по вложенности до района стоит дорого, чтобы делать его на каждом
    // ребре, поэтому ответ запоминается на зону в пределах ОДНОГО поиска: район у соседних мест общий.
    if (policy.avoid_faction != 0 || policy.crime_weight > 0.0f) {
      auto known = query->control_cache->find(to.zone);
      if (known == query->control_cache->end()) {
        zone_control value{};
        value.crime = query->store->control_at(to.zone, control_field::crime).crime;
        value.faction = query->store->control_at(to.zone, control_field::faction).faction;
        known = query->control_cache->emplace(to.zone, value).first;
      }
      if (policy.crime_weight > 0.0f) {
        step *= 1.0f + policy.crime_weight * float(known->second.crime) / 1000.0f;
      }
      if (policy.avoid_faction != 0 && known->second.faction == policy.avoid_faction) {
        step *= policy.avoid_cost;
      }
    }

    // Чужая частная территория дорожает шаг. Спрашивается тот же `may_enter`, которым игра решает, звать
    // ли стражу: маршрут и правоприменение обязаны опираться на одно правило, иначе персонаж пойдёт
    // там, где его за это арестуют.
    if (policy.realm != nullptr && !may_enter(*policy.realm, to.zone, policy.actor).allowed) {
      step *= policy.trespass_cost;
    }
    return step;
  }

  astar<part_ref>::float_t goal_cost(const part_ref&, const part_ref&, const void*) const override {
    return 0.0;
  }

  bool is_same(const part_ref& a, const part_ref& b, const void*) const override { return a == b; }

  void fill_successors(astar<part_ref>::container* container, const part_ref& here,
                       const void* payload) const override {
    const auto* query = static_cast<const path_query*>(payload);
    for (const auto& portal : query->store->portals_of(here)) {
      if (!portal.passable() || portal.other == invalid_key) continue;

      const part_ref next{portal.other, portal.other_part};
      // Проходимость спрашивается У ХРАНИЛИЩА, а не у записи: закрытая дверь — рантайм-состояние МЕСТА,
      // и путь обязан меняться от того, что её заперли, без пересборки файлов.
      const auto* zone = query->store->find(next.zone);
      if (zone == nullptr || !query->store->passable(*zone)) continue;
      if (query->store->part_of(next) == nullptr) continue; // сосед в невыгруженном секторе
      container->add_successor(next);
    }
  }
};

std::vector<part_ref> find_path(const zone_store& store, const part_ref from, const part_ref to,
                                const uint32_t budget, const travel_policy& policy) {
  if (from == to) return {};
  if (store.part_of(from) == nullptr || store.part_of(to) == nullptr) return {};

  const auto* goal_zone = store.find(to.zone);
  if (goal_zone == nullptr || !store.passable(*goal_zone)) return {};

  std::map<zone_key, zone_control> control_cache;
  const path_query query{&store, &policy, &control_cache};

  static const path_rules rules;
  astar<part_ref>::container container;
  astar<part_ref>::algorithm search(&container, &rules, from, to, &query);

  auto state = astar<part_ref>::state::searching;
  for (uint32_t step = 0; step < budget && state == astar<part_ref>::state::searching; ++step) {
    state = search.step();
  }
  g_last_search_nodes = search.step_count();

  if (state != astar<part_ref>::state::succeeded) {
    if (state == astar<part_ref>::state::searching) {
      search.cancel();
      search.step();
    }
    return {};
  }

  // Первый узел решения — исходная часть, её в путь не кладём: `step_agent` ждёт список ОСТАВШИХСЯ.
  std::vector<part_ref> path;
  for (const auto* node : search.solution()) {
    path.push_back(node->data);
  }
  search.free_solution();

  if (!path.empty()) path.erase(path.begin());
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
