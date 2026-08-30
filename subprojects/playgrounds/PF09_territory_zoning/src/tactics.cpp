#include "tactics.h"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include "navigate.h"

namespace devils_engine::pf09 {

namespace {

constexpr float sight_step_m = 0.35f;   // шаг выборки вдоль луча: мельче габарита актора
constexpr float cover_depth_m = 0.55f;  // насколько отступить за предмет, чтобы он действительно закрывал

// В каком месте лежит точка. Место состоит из выпуклых частей, поэтому проверка — перебор частей, а не
// один многоугольник; частей у места единицы, и городить пространственный индекс здесь не за что.
bool inside_place(const zone_store& store, const zone_key place, const glm::vec2 point, uint32_t& part) {
  const auto* record = store.find(place);
  if (record == nullptr) return false;

  for (uint32_t index = 0; index < record->part_count; ++index) {
    const auto outline = store.outline_of({place, index});
    if (point_in_outline(outline, point)) {
      part = index;
      return true;
    }
  }
  return false;
}

bool crosses_prop(const std::span<const zone_prop> props, const glm::vec2 from, const glm::vec2 to) {
  const auto span = to - from;
  const float length = glm::dot(span, span);

  for (const auto& prop : props) {
    if (!prop.blocks_sight()) continue;
    const float t = length < 1.0e-8f ? 0.0f : std::clamp(glm::dot(prop.position - from, span) / length, 0.0f, 1.0f);
    const auto closest = from + span * t;
    const auto delta = prop.position - closest;
    if (delta.x * delta.x + delta.y * delta.y < prop.radius * prop.radius) return true;
  }
  return false;
}

} // namespace

bool visible(const zone_store& store, const zone_key place, const glm::vec2 from, const glm::vec2 to) {
  uint32_t part = 0;
  if (!inside_place(store, place, from, part) || !inside_place(store, place, to, part)) return false;
  if (crosses_prop(store.props_of(place), from, to)) return false;

  // Луч обязан ВСЁ ВРЕМЯ оставаться внутри места. Место невыпукло (оно из частей), поэтому проверить
  // концы мало: отрезок между двумя комнатами буквой «Г» уходит наружу через стену, а концы при этом
  // внутри. Выборка идёт шагом мельче габарита актора — щели, в которую человек не пролезет, тут и не
  // должно хватать на обман.
  const auto span = to - from;
  const float distance = std::sqrt(span.x * span.x + span.y * span.y);
  const uint32_t steps = uint32_t(distance / sight_step_m) + 1;
  for (uint32_t i = 1; i < steps; ++i) {
    const auto sample = from + span * (float(i) / float(steps));
    if (!inside_place(store, place, sample, part)) return false;
  }
  return true;
}

bool settle_into_place(const zone_store& store, const zone_key place, glm::vec2& point) {
  uint32_t part = 0;
  if (inside_place(store, place, point, part)) return true;

  const auto* record = store.find(place);
  if (record == nullptr) return false;

  // Подтягиваем к середине ближайшей части: место невыпукло, и «к центру места» увело бы точку сквозь
  // стену в соседнюю комнату той же зоны.
  for (uint32_t index = 0; index < record->part_count; ++index) {
    glm::vec2 middle{};
    if (!interior_point(store, {place, index}, middle)) continue;
    for (uint32_t attempt = 0; attempt < 6; ++attempt) {
      const auto probe = point + (middle - point) * (0.15f * float(attempt + 1));
      if (inside_place(store, place, probe, part)) {
        point = probe;
        return true;
      }
    }
  }
  return false;
}

std::vector<tactical_spot> cover_spots(const zone_store& store, const zone_key place, const glm::vec2 threat,
                                       const uint32_t limit) {
  std::vector<tactical_spot> out;
  const auto* record = store.find(place);
  if (record == nullptr) return out;

  const auto props = store.props_of(place);
  for (uint32_t index = 0; index < props.size(); ++index) {
    const auto& prop = props[index];
    if (!prop.blocks_sight()) continue;

    auto away = prop.position - threat;
    const float length = std::sqrt(away.x * away.x + away.y * away.y);
    if (length < 1.0e-3f) continue;
    away /= length;

    const auto spot = prop.position + away * (prop.radius + cover_depth_m);

    uint32_t part = 0;
    if (!inside_place(store, place, spot, part)) continue;
    if (blocked_by_prop(store, {place, part}, spot)) continue;

    // Главное: укрытие ПРОВЕРЯЕТСЯ, а не выводится. «За предметом» — это построение; «не видно» — факт,
    // и только он имеет значение. Угроза за спиной у предмета сводит первое к нулю, не трогая второе.
    if (visible(store, place, threat, spot)) continue;

    // Кто именно укрывает. Если подход к предмету СО СТОРОНЫ УГРОЗЫ тоже не просматривается, то предмет
    // здесь ни при чём: точка спрятана формой места — за углом, в соседней комнате той же зоны. Записать
    // в таком случае предмет значило бы соврать игре, которая по этому полю решает, что можно опрокинуть
    // или поджечь, чтобы укрытия не стало. Укрытие остаётся, авторство — нет.
    auto front_direction = threat - prop.position;
    const float front_length = std::sqrt(front_direction.x * front_direction.x +
                                         front_direction.y * front_direction.y);
    bool by_object = false;
    if (front_length > 1.0e-3f) {
      const auto front = prop.position + front_direction / front_length * (prop.radius + cover_depth_m);
      by_object = visible(store, place, threat, front);
    }

    out.push_back({spot, part, by_object ? index : 0xffffffffu, length});
  }

  // Ближние укрытия полезнее: до дальнего ещё надо дойти под огнём.
  std::sort(out.begin(), out.end(), [](const tactical_spot& a, const tactical_spot& b) { return a.score < b.score; });
  if (out.size() > limit) out.resize(limit);
  return out;
}

std::vector<tactical_spot> watch_spots(const zone_store& store, const zone_key place, const uint32_t limit) {
  std::vector<tactical_spot> out;
  const auto* record = store.find(place);
  if (record == nullptr) return out;

  // Что именно надо видеть: внешние проёмы места. Это и есть его входы — то, откуда приходят.
  std::vector<glm::vec2> gates;
  for (const auto& portal : store.perimeter(place)) {
    gates.push_back(portal.middle());
  }
  if (gates.empty()) return out;

  // Кандидаты: свободные точки частей плюс середины между ними. Больше нам и не нужно — место маленькое,
  // а «идеальная» точка наблюдения здесь не считается: считается покрытие входов.
  std::vector<tactical_spot> candidates;
  for (uint32_t index = 0; index < record->part_count; ++index) {
    glm::vec2 point{};
    if (interior_point(store, {place, index}, point)) candidates.push_back({point, index, 0xffffffffu, 0.0f});
  }
  if (candidates.empty()) return out;

  std::vector<bool> covered(gates.size(), false);
  uint32_t remaining = uint32_t(gates.size());

  while (out.size() < limit && remaining > 0) {
    int32_t best = -1;
    uint32_t best_gain = 0;
    for (uint32_t i = 0; i < candidates.size(); ++i) {
      uint32_t gain = 0;
      for (uint32_t g = 0; g < gates.size(); ++g) {
        if (covered[g]) continue;
        if (visible(store, place, candidates[i].position, gates[g])) ++gain;
      }
      if (gain > best_gain) {
        best_gain = gain;
        best = int32_t(i);
      }
    }
    if (best < 0) break; // оставшиеся входы не видны ниоткуда — законный ответ, а не повод крутиться

    auto chosen = candidates[size_t(best)];
    chosen.score = float(best_gain);
    for (uint32_t g = 0; g < gates.size(); ++g) {
      if (covered[g] || !visible(store, place, chosen.position, gates[g])) continue;
      covered[g] = true;
      --remaining;
    }
    out.push_back(chosen);
    candidates.erase(candidates.begin() + best);
    if (candidates.empty()) break;
  }
  return out;
}

std::vector<order_assignment> fan_out(const zone_store& store, const zone_key place, const order_kind kind,
                                      const glm::vec2 threat, const std::vector<glm::vec2>& agents) {
  std::vector<order_assignment> out;
  if (agents.empty()) return out;

  auto spots = kind == order_kind::cover ? cover_spots(store, place, threat, uint32_t(agents.size()))
                                         : watch_spots(store, place, uint32_t(agents.size()));

  // Укрытий или точек наблюдения может не хватить на всех. Добивать «куда-нибудь» нельзя: человек,
  // отправленный в никуда, хуже человека, оставшегося на месте. Поэтому добавляем свободные точки частей
  // — они хотя бы гарантированно достижимы и не заняты.
  const auto* record = store.find(place);
  if (record != nullptr) {
    for (uint32_t index = 0; index < record->part_count && spots.size() < agents.size(); ++index) {
      glm::vec2 point{};
      if (!interior_point(store, {place, index}, point)) continue;
      const bool taken = std::any_of(spots.begin(), spots.end(), [&](const tactical_spot& item) {
        const auto delta = item.position - point;
        return delta.x * delta.x + delta.y * delta.y < 1.0f;
      });
      if (!taken) spots.push_back({point, index, 0xffffffffu, 0.0f});
    }
  }
  if (spots.empty()) return out;

  // Раздача: каждый следующий свободный человек забирает ближайшую ещё не занятую точку. Жадно и
  // устойчиво; «оптимальное» назначение здесь стоило бы дороже, чем стоит разница.
  std::vector<bool> used(spots.size(), false);
  for (uint32_t agent = 0; agent < agents.size(); ++agent) {
    int32_t best = -1;
    float best_distance = 1.0e30f;
    for (uint32_t i = 0; i < spots.size(); ++i) {
      if (used[i]) continue;
      const auto delta = spots[i].position - agents[agent];
      const float distance = delta.x * delta.x + delta.y * delta.y;
      if (distance < best_distance) {
        best_distance = distance;
        best = int32_t(i);
      }
    }
    if (best < 0) break; // точек меньше, чем людей: остальные остаются при своём

    used[size_t(best)] = true;
    const auto& spot = spots[size_t(best)];
    out.push_back({agent, part_ref{place, spot.part}, spot.position, spot.sheltered()});
  }
  return out;
}

} // namespace devils_engine::pf09
