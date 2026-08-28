#include "terrain.h"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

namespace devils_engine::pf08 {
namespace {

double smooth_step(const double edge0, const double edge1, const double value) {
  const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

} // namespace

double valley_height(const double x, const double z) {
  // Ось прохода гуляет: прямой коридор читается как стена с прорезью, а не как долина.
  const double axis = 15.0 * std::sin(z * 0.0125) + 6.0 * std::sin(z * 0.031 + 1.3);
  const double across = std::abs(x - axis);

  // Гребни по бокам. Подъём начинается не от самой оси: у долины есть ровное дно, иначе некуда
  // ставить предметы и незачем идти по проходу.
  constexpr double floor_width = 26.0;
  constexpr double ridge_span = 60.0;
  constexpr double ridge_height = 18.0;
  double ridge = smooth_step(floor_width, floor_width + ridge_span, across) * ridge_height;
  // Гребень неровный вдоль себя: перевалы и седловины. Без этого он выглядит вытянутым валом.
  ridge *= 0.72 + 0.28 * std::sin(z * 0.018 + 0.7) * std::cos(x * 0.011);

  // Мелкая неровность по всей долине, включая дно.
  const double bumps = 0.55 * std::sin(x * 0.085) * std::cos(z * 0.097) +
                       0.26 * std::sin(x * 0.21 + 2.0) * std::sin(z * 0.17);

  // К краю участка рельеф выходит в ноль: дальше начинается ровный диск до горизонта, и ступеньки
  // на стыке быть не должно.
  const double radius = std::sqrt(x * x + z * z);
  const double fade = 1.0 - smooth_step(valley_fade_start, valley_fade_end, radius);
  return (ridge + bumps) * fade;
}

glm::vec3 valley_normal(const double x, const double z) {
  // Градиент центральными разностями. Аналитическая производная здесь дала бы длинное выражение,
  // которое пришлось бы править вместе с каждой правкой рельефа и которое некому проверить.
  constexpr double step = 0.35;
  const double dx = (valley_height(x + step, z) - valley_height(x - step, z)) / (2.0 * step);
  const double dz = (valley_height(x, z + step) - valley_height(x, z - step)) / (2.0 * step);
  return glm::normalize(glm::vec3(float(-dx), 1.0f, float(-dz)));
}

std::vector<scene_vertex> make_valley_patch() {
  const int32_t cells = int32_t(std::lround(valley_half_size * 2.0 / valley_cell));
  std::vector<scene_vertex> out;
  out.reserve(size_t(cells) * size_t(cells) * 6);

  const auto emit = [&out](const double x, const double z) {
    const double y = valley_height(x, z);
    const glm::vec3 normal = valley_normal(x, z);
    out.push_back(scene_vertex{float(x), float(y), float(z), normal.x, normal.y, normal.z});
  };

  for (int32_t iz = 0; iz < cells; ++iz) {
    for (int32_t ix = 0; ix < cells; ++ix) {
      const double x0 = -valley_half_size + double(ix) * valley_cell;
      const double z0 = -valley_half_size + double(iz) * valley_cell;
      const double x1 = x0 + valley_cell;
      const double z1 = z0 + valley_cell;
      emit(x0, z0);
      emit(x1, z1);
      emit(x1, z0);
      emit(x0, z0);
      emit(x0, z1);
      emit(x1, z1);
    }
  }
  return out;
}

} // namespace devils_engine::pf08
