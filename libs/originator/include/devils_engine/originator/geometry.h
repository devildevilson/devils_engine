#ifndef DEVILS_ENGINE_ORIGINATOR_GEOMETRY_H
#define DEVILS_ENGINE_ORIGINATOR_GEOMETRY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <utility>

// Общая обрезка треугольника по полуоткрытой коробке. Тип точки предоставляет x/y/z и
// конструируется из трёх double; порядок обхода и пересечений фиксирован, чтобы соседние
// чанки получали одинаковые вершины на общей грани. Буфер на 12 точек покрывает шесть плоскостей.

namespace devils_engine::originator {

template <class Point>
struct clipped_polygon3 {
  std::array<Point, 12> vertices{};
  size_t count = 0;
};

template <class Point>
bool triangle_box_bounds_overlap(const Point first, const Point second, const Point third,
                                 const Point low, const Point high) {
  return !(std::max({first.x, second.x, third.x}) < low.x || std::min({first.x, second.x, third.x}) >= high.x ||
      std::max({first.y, second.y, third.y}) < low.y || std::min({first.y, second.y, third.y}) >= high.y ||
      std::max({first.z, second.z, third.z}) < low.z || std::min({first.z, second.z, third.z}) >= high.z);
}

template <class Point>
clipped_polygon3<Point> clip_triangle_to_box(const Point first, const Point second, const Point third,
                                             const Point low, const Point high) {
  clipped_polygon3<Point> result;
  if (!triangle_box_bounds_overlap(first, second, third, low, high)) return result;
  std::array<Point, 12> spare{};
  auto& polygon = result.vertices;
  polygon[0] = first; polygon[1] = second; polygon[2] = third;
  result.count = 3;
  for (size_t axis = 0; axis < 3 && result.count; ++axis) {
    for (const bool upper : {false, true}) {
      const auto coordinate = [axis](const Point& p) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; };
      const double plane = axis == 0 ? (upper ? high.x : low.x) : axis == 1 ? (upper ? high.y : low.y)
                                                                              : (upper ? high.z : low.z);
      const auto inside = [&](const Point& p) { return upper ? coordinate(p) < plane : coordinate(p) >= plane; };
      size_t out = 0;
      for (size_t i = 0; i < result.count; ++i) {
        const Point& p = polygon[i];
        const Point& q = polygon[(i + 1) % result.count];
        const bool pin = inside(p), qin = inside(q);
        if (pin) spare[out++] = p;
        if (pin != qin) {
          const double t = (plane - coordinate(p)) / (coordinate(q) - coordinate(p));
          spare[out++] = Point{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t};
        }
      }
      result.count = out;
      std::copy(spare.begin(), spare.begin() + out, polygon.begin());
      if (!out) break;
    }
  }
  return result;
}

// Кратчайшее расстояние от отрезка до осевого ящика. Расстояние до выпуклого
// ящика вдоль отрезка выпукло, поэтому после грубой оценки хватает одномерного
// поиска; threshold позволяет не начинать его для заведомо далёкого отрезка.
template <class Point>
double distance_point_to_box(const Point p, const Point low, const Point high) {
  const double x = std::max({low.x - p.x, 0.0, p.x - high.x});
  const double y = std::max({low.y - p.y, 0.0, p.y - high.y});
  const double z = std::max({low.z - p.z, 0.0, p.z - high.z});
  return std::sqrt(x * x + y * y + z * z);
}

template <class Point>
double distance_segment_to_box(const Point a, const Point b, const Point low, const Point high,
                               const double threshold) {
  const auto add = [](const Point p, const Point q) { return Point{p.x + q.x, p.y + q.y, p.z + q.z}; };
  const auto sub = [](const Point p, const Point q) { return Point{p.x - q.x, p.y - q.y, p.z - q.z}; };
  const auto scale = [](const Point p, const double s) { return Point{p.x * s, p.y * s, p.z * s}; };
  const auto dot = [](const Point p, const Point q) { return p.x * q.x + p.y * q.y + p.z * q.z; };
  const auto length = [&](const Point p) { return std::sqrt(dot(p, p)); };
  const Point centre = scale(add(low, high), 0.5);
  const double radius = length(sub(high, low)) / 2.0;
  const Point ab = sub(b, a);
  const double span = dot(ab, ab);
  const double t = span > 0.0 ? std::clamp(dot(sub(centre, a), ab) / span, 0.0, 1.0) : 0.0;
  const double coarse = length(sub(centre, add(a, scale(ab, t)))) - radius;
  if (coarse >= threshold) return coarse;
  const auto at = [&](const double u) { return distance_point_to_box(add(a, scale(ab, u)), low, high); };
  double left = 0.0, right = 1.0;
  for (int i = 0; i < 40; ++i) {
    const double m1 = left + (right - left) / 3.0, m2 = right - (right - left) / 3.0;
    (at(m1) <= at(m2) ? right : left) = (at(m1) <= at(m2) ? m2 : m1);
  }
  return std::min({at(0.0), at(1.0), at((left + right) / 2.0)});
}

// Четыре четверти клетки в горизонтальной плоскости задают твёрдый объём. Генератор
// предоставляет выборку соседей, преобразование координат и материал; библиотека
// выводит открытые грани в фиксированном порядке без привязки к конкретной постройке.
template <class Point, class Sample, class Shade, class World, class Normal, class Emit>
void mesh_quarter_voxels(const int32_t i0, const int32_t k0, const int32_t y0,
                         const int32_t width, const int32_t depth, const int32_t height,
                         const double cell, Sample sample, Shade shade_at, World world,
                         Normal local_normal, Emit emit) {
  static constexpr int32_t side_u[4] = {-1, 0, 1, 0}, side_v[4] = {0, 1, 0, -1};
  for (int32_t y = y0; y < y0 + height; ++y) {
    for (int32_t k = k0; k < k0 + depth; ++k) {
      for (int32_t i = i0; i < i0 + width; ++i) {
        const uint8_t q = sample(i, y, k);
        if (!q) continue;
        const uint8_t shade = shade_at(i, y, k);
        const double u0 = i * cell, u1 = u0 + cell;
        const double v0 = k * cell, v1 = v0 + cell;
        const double low_y = y * cell, high_y = low_y + cell;
        const double uc = (u0 + u1) / 2.0, vc = (v0 + v1) / 2.0;
        const double corner_u[4] = {u0, u1, u1, u0}, corner_v[4] = {v1, v1, v0, v0};
        const auto side_ends = [&](const uint32_t s) {
          return std::pair{std::pair{corner_u[(s + 3) % 4], corner_v[(s + 3) % 4]},
                           std::pair{corner_u[s], corner_v[s]}};
        };
        for (uint32_t s = 0; s < 4; ++s) {
          if (!(q & (1u << s))) continue;
          const auto [p, r] = side_ends(s);
          const uint8_t neighbour = sample(i + side_u[s], y, k + side_v[s]);
          if (!(neighbour & (1u << ((s + 2) % 4)))) {
            const Point normal = local_normal(side_u[s], 0, side_v[s]);
            const Point a = world(p.first, low_y, p.second), b = world(r.first, low_y, r.second);
            const Point c = world(r.first, high_y, r.second), d = world(p.first, high_y, p.second);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          const uint32_t t = (s + 1) % 4;
          if (!(q & (1u << t))) {
            const double nu = side_u[t] - side_u[s], nv = side_v[t] - side_v[s];
            const Point normal = local_normal(nu / std::sqrt(2.0), 0, nv / std::sqrt(2.0));
            const Point a = world(uc, low_y, vc), b = world(corner_u[s], low_y, corner_v[s]);
            const Point c = world(corner_u[s], high_y, corner_v[s]), d = world(uc, high_y, vc);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          const uint32_t back = (s + 3) % 4;
          if (!(q & (1u << back))) {
            const double nu = side_u[back] - side_u[s], nv = side_v[back] - side_v[s];
            const Point normal = local_normal(nu / std::sqrt(2.0), 0, nv / std::sqrt(2.0));
            const Point a = world(uc, low_y, vc), b = world(corner_u[back], low_y, corner_v[back]);
            const Point c = world(corner_u[back], high_y, corner_v[back]), d = world(uc, high_y, vc);
            emit(a, b, c, normal, shade);
            emit(a, c, d, normal, shade);
          }
          if (!(sample(i, y + 1, k) & (1u << s))) {
            emit(world(uc, high_y, vc), world(p.first, high_y, p.second), world(r.first, high_y, r.second),
                 Point{0, 1, 0}, shade);
          }
          if (!(sample(i, y - 1, k) & (1u << s))) {
            emit(world(uc, low_y, vc), world(p.first, low_y, p.second), world(r.first, low_y, r.second),
                 Point{0, -1, 0}, shade);
          }
        }
      }
    }
  }
}

} // namespace devils_engine::originator

#endif
