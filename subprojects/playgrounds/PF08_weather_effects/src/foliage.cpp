#include "foliage.h"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "terrain.h"

namespace devils_engine::pf08 {
namespace {

constexpr double pi = 3.14159265358979323846;

// Детерминированный хеш: посадка обязана быть одинаковой в любом запуске и в любом дампе, иначе
// сравнивать кадры между собой невозможно. Генератор из <random> дал бы то же самое, но потянул бы
// за собой состояние, которого здесь быть не должно.
double hash01(const uint32_t seed) {
  uint32_t value = seed * 747796405u + 2891336453u;
  value = ((value >> ((value >> 28) + 4)) ^ value) * 277803737u;
  value = (value >> 22) ^ value;
  return double(value) * (1.0 / 4294967296.0);
}

void add_blade(std::vector<scene_vertex>& out, const double yaw, const double lean, const double width,
               const double curve) {
  const double cos_yaw = std::cos(yaw);
  const double sin_yaw = std::sin(yaw);
  const auto place = [&](const double along, const double side) {
    // Лезвие сужается к верхушке и отгибается наружу: прямая полоска читается спичкой.
    const double taper = width * (1.0 - 0.85 * along);
    const double bend = lean * along * along;
    const double local_x = side * taper + bend;
    const double local_z = curve * along * along;
    return glm::dvec3(local_x * cos_yaw - local_z * sin_yaw, along,
                      local_x * sin_yaw + local_z * cos_yaw);
  };

  const glm::dvec3 base_left = place(0.0, -1.0);
  const glm::dvec3 base_right = place(0.0, 1.0);
  const glm::dvec3 tip_left = place(1.0, -1.0);
  const glm::dvec3 tip_right = place(1.0, 1.0);
  // Нормаль лезвия перпендикулярна его плоскости.
  const glm::dvec3 normal =
    glm::normalize(glm::cross(tip_left - base_left, base_right - base_left));

  const auto push = [&out](const glm::dvec3& p, const glm::dvec3& n) {
    out.push_back(scene_vertex{float(p.x), float(p.y), float(p.z), float(n.x), float(n.y), float(n.z)});
  };
  push(base_left, normal);
  push(base_right, normal);
  push(tip_right, normal);
  push(base_left, normal);
  push(tip_right, normal);
  push(tip_left, normal);
  // Обратная сторона: тот же прямоугольник с обратным обходом и вывернутой нормалью.
  push(base_left, -normal);
  push(tip_right, -normal);
  push(base_right, -normal);
  push(base_left, -normal);
  push(tip_left, -normal);
  push(tip_right, -normal);
}

} // namespace

std::vector<scene_vertex> make_shrub(const uint32_t blade_count) {
  std::vector<scene_vertex> out;
  out.reserve(size_t(blade_count) * 12);
  for (uint32_t index = 0; index < blade_count; ++index) {
    const double fraction = double(index) / double(std::max(blade_count, 1u));
    const double yaw = fraction * 2.0 * pi + 0.4 * hash01(index * 17u + 3u);
    const double lean = 0.18 + 0.22 * hash01(index * 31u + 11u);
    const double width = 0.075 + 0.045 * hash01(index * 53u + 7u);
    const double curve = -0.10 + 0.20 * hash01(index * 71u + 19u);
    add_blade(out, yaw, lean, width, curve);
  }
  return out;
}

std::vector<shrub> scatter_shrubs(const uint32_t target_count) {
  std::vector<shrub> out;
  out.reserve(target_count);

  for (uint32_t index = 0; index < target_count * 3 && out.size() < target_count; ++index) {
    const double u = hash01(index * 2u + 1u);
    const double v = hash01(index * 2u + 977u);
    const double x = (u * 2.0 - 1.0) * valley_fade_start;
    const double z = (v * 2.0 - 1.0) * valley_fade_start;

    const double height = valley_height(x, z);
    const glm::vec3 normal = valley_normal(x, z);
    // На крутом склоне и на гребне голо. Это не только про вид: заросли на склоне торчали бы из него
    // под углом, потому что куст ставится вертикально, а не по нормали.
    if (normal.y < 0.86f) continue;
    const double bareness = std::clamp((height - 5.0) / 8.0, 0.0, 1.0);
    if (hash01(index * 7919u + 13u) < bareness) continue;

    shrub item{};
    item.x = float(x);
    item.y = float(height);
    item.z = float(z);
    const double size = 0.55 + 0.85 * hash01(index * 104729u + 5u);
    item.height = float(size);
    item.width = float(size * (0.7 + 0.5 * hash01(index * 15485863u + 23u)));
    item.yaw = float(hash01(index * 32452843u + 29u) * 2.0 * pi);
    // Фаза ветра у каждого куста своя, иначе все качаются как один предмет.
    item.phase = float(hash01(index * 49979687u + 31u) * 2.0 * pi);
    const float tone = float(0.75 + 0.5 * hash01(index * 86028121u + 37u));
    // Средняя яркость прежнего (0.20, 0.30, 0.13) была в 2.66 раза выше земли. При лунном свете и
    // ночном зрении куст превращался в светлую серую массу даже там, где силуэт должен теряться.
    item.albedo = glm::vec3(0.13f, 0.20f, 0.08f) * tone;
    out.push_back(item);
  }
  return out;
}

} // namespace devils_engine::pf08
