#include "fixture.h"

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace devils_engine::pf07 {
namespace {

void add_quad(std::vector<scene_vertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
              const glm::vec3& d, const glm::vec3& normal) {
  const auto push = [&out, normal](const glm::vec3& p) {
    out.push_back(scene_vertex{p.x, p.y, p.z, normal.x, normal.y, normal.z});
  };
  push(a);
  push(b);
  push(c);
  push(a);
  push(c);
  push(d);
}

scene_instance box(const glm::vec3 centre, const glm::vec3 half, const glm::vec3 albedo) noexcept {
  return scene_instance{glm::vec4(centre, 0.0f), glm::vec4(half, 0.0f), glm::vec4(albedo, 0.0f)};
}

// Столб стоит НА земле, поэтому центр поднимается на половину высоты: иначе половина каждого предмета
// уходит под поверхность, и тень начинается не от его основания.
scene_instance pillar(const float x, const float z, const float height, const float thickness,
                      const glm::vec3 albedo) noexcept {
  return box({x, height * 0.5f, z}, {thickness * 0.5f, height * 0.5f, thickness * 0.5f}, albedo);
}

} // namespace

std::vector<scene_vertex> make_unit_cube() {
  std::vector<scene_vertex> out;
  constexpr float n = -0.5f;
  constexpr float p = 0.5f;
  add_quad(out, {n, n, p}, {p, n, p}, {p, p, p}, {n, p, p}, {0, 0, 1});
  add_quad(out, {p, n, n}, {n, n, n}, {n, p, n}, {p, p, n}, {0, 0, -1});
  add_quad(out, {p, n, p}, {p, n, n}, {p, p, n}, {p, p, p}, {1, 0, 0});
  add_quad(out, {n, n, n}, {n, n, p}, {n, p, p}, {n, p, n}, {-1, 0, 0});
  add_quad(out, {n, p, p}, {p, p, p}, {p, p, n}, {n, p, n}, {0, 1, 0});
  add_quad(out, {n, n, n}, {p, n, n}, {p, n, p}, {n, n, p}, {0, -1, 0});
  return out;
}

std::vector<scene_instance> make_fixture_instances() {
  // Альбедо намеренно нейтральные и близкие друг к другу: цвет теней в этом срезе обязан приходить от
  // СВЕТИЛ, а не от краски предметов. Разными их делает только светлота, чтобы предметы отличались.
  const glm::vec3 pale{0.62f, 0.60f, 0.57f};
  const glm::vec3 mid{0.44f, 0.43f, 0.41f};
  const glm::vec3 dark{0.28f, 0.28f, 0.27f};

  std::vector<scene_instance> out;
  // Ряд столбов разной высоты поперёк вида: разная длина тени в одном кадре сразу показывает, держит
  // ли каскад масштаб, и делит ли кросс-фейд обе тени одинаково на всех расстояниях.
  out.push_back(pillar(-6.0f, 12.0f, 4.0f, 0.7f, pale));
  out.push_back(pillar(-1.5f, 14.0f, 5.5f, 0.6f, mid));
  out.push_back(pillar(3.0f, 11.0f, 3.0f, 0.8f, pale));
  out.push_back(pillar(8.5f, 15.0f, 6.5f, 0.9f, mid));
  out.push_back(pillar(-11.0f, 18.0f, 4.5f, 0.7f, dark));

  // Дальняя группа: тени отсюда попадают уже во второй-третий каскад, и стык между ними виден именно
  // на длинной тени, а не на предмете.
  out.push_back(pillar(-4.0f, 34.0f, 7.0f, 1.1f, mid));
  out.push_back(pillar(6.0f, 38.0f, 5.0f, 1.0f, pale));
  out.push_back(pillar(16.0f, 30.0f, 8.0f, 1.2f, dark));

  // Плиты-приёмники. Тень, упавшая на другой предмет, ловит ошибки глубины и смещения, которых ровная
  // земля не показывает вовсе.
  out.push_back(box({0.0f, 0.35f, 20.0f}, {6.0f, 0.35f, 2.5f}, pale));
  out.push_back(box({-9.0f, 0.6f, 25.0f}, {3.0f, 0.6f, 3.0f}, mid));
  out.push_back(box({11.0f, 1.1f, 22.0f}, {2.0f, 1.1f, 2.0f}, dark));

  // Стена поперёк: единственный предмет, у которого есть заметная боковая грань. На ней видно, как
  // свет двух светил разного цвета ложится на одну плоскость под разными углами.
  out.push_back(box({2.0f, 1.6f, 45.0f}, {9.0f, 1.6f, 0.6f}, pale));
  return out;
}

namespace {

// Просадка поверхности сферы на горизонтальном удалении. Наивное `R - sqrt(R*R - d*d)` вычитает два
// почти равных числа: на шести километрах при радиусе в шесть тысяч километров ответ равен 2.8 м из
// величины порядка шести миллионов. Тождество `d² / (R + sqrt(R² - d²))` даёт то же число без
// вычитания близких величин — тот же приём, что спас край тени на аналитической земле.
double surface_drop(const double distance, const double planet_radius) {
  const double inner = std::sqrt(std::max(0.0, planet_radius * planet_radius - distance * distance));
  return distance * distance / (planet_radius + inner);
}

} // namespace

std::vector<scene_vertex> make_ground_disc(const double radius_m, const double planet_radius_m,
                                           const uint32_t rings, const uint32_t segments) {
  std::vector<scene_vertex> out;
  out.reserve(size_t(rings) * segments * 6 + size_t(segments) * 3);

  // Кольца идут ГЕОМЕТРИЧЕСКОЙ прогрессией: у камеры густо, вдали редко. Равномерный шаг тратил бы
  // почти все кольца на дальние километры, где вся земля занимает несколько строк пикселей.
  const double inner_radius = 1.0;
  const auto ring_radius = [&](const uint32_t index) {
    if (index == 0) return 0.0;
    const double fraction = double(index) / double(rings);
    return inner_radius * std::pow(radius_m / inner_radius, fraction);
  };

  const auto vertex = [&](const double distance, const double angle) {
    const double x = distance * std::cos(angle);
    const double z = distance * std::sin(angle);
    const double y = -surface_drop(distance, planet_radius_m);
    // Нормаль — направление от ЦЕНТРА ПЛАНЕТЫ, который лежит на радиус ниже наблюдателя. На шести
    // километрах она отклоняется от вертикали на 0.05°: величина крошечная, но считать её отдельно
    // не дороже, чем писать единицу, а физика поверхности сферы остаётся честной.
    const glm::dvec3 normal = glm::normalize(glm::dvec3(x, planet_radius_m + y, z));
    return scene_vertex{float(x), float(y), float(z),
                        float(normal.x), float(normal.y), float(normal.z)};
  };

  for (uint32_t ring = 0; ring < rings; ++ring) {
    const double near_radius = ring_radius(ring);
    const double far_radius = ring_radius(ring + 1);
    for (uint32_t segment = 0; segment < segments; ++segment) {
      const double a0 = 2.0 * 3.14159265358979323846 * double(segment) / double(segments);
      const double a1 = 2.0 * 3.14159265358979323846 * double(segment + 1) / double(segments);
      if (ring == 0) {
        // Центральная шапка: треугольники сходятся в одну точку, четвёртой вершины у них нет.
        out.push_back(vertex(0.0, 0.0));
        out.push_back(vertex(far_radius, a1));
        out.push_back(vertex(far_radius, a0));
        continue;
      }
      out.push_back(vertex(near_radius, a0));
      out.push_back(vertex(far_radius, a1));
      out.push_back(vertex(far_radius, a0));
      out.push_back(vertex(near_radius, a0));
      out.push_back(vertex(near_radius, a1));
      out.push_back(vertex(far_radius, a1));
    }
  }
  return out;
}

scene_instance make_ground_instance(const double centre_x, const double centre_z,
                                    const glm::vec3& albedo) {
  // Половинные размеры равны половине: вершинный шейдер умножает положение на удвоенный полуразмер,
  // и половина возвращает диску его собственный масштаб в метрах.
  return scene_instance{glm::vec4(float(centre_x), 0.0f, float(centre_z), 0.0f),
                        glm::vec4(0.5f, 0.5f, 0.5f, 0.0f), glm::vec4(albedo, 0.0f)};
}

} // namespace devils_engine::pf07
