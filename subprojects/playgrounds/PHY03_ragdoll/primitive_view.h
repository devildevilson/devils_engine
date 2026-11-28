#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "simulation.h"

struct GLFWwindow;

namespace devils_engine::phy03::primitive_view {

struct vertex {
  glm::vec4 position;
  glm::vec4 color;
};

class renderer {
public:
  renderer(GLFWwindow* window, const std::string& title, bool validation);
  ~renderer();

  bool draw(const glm::mat4& view, uint32_t width, uint32_t height,
            std::span<const vertex> vertices, uint32_t solid_count);

private:
  struct state;
  std::unique_ptr<state> data;
};

using JPH::Vec3;
constexpr float pi = std::numbers::pi_v<float>;
using triangle = std::array<Vec3, 3>;
using line = std::array<Vec3, 2>;
constexpr int round_sides = 12;
constexpr int round_bands = 4;
using round_rings = std::array<std::array<Vec3, round_sides>, 2 * (round_bands + 1)>;

inline void quad(std::vector<triangle>& mesh, const Vec3 a, const Vec3 b, const Vec3 c, const Vec3 d) {
  mesh.push_back({a, b, c});
  mesh.push_back({a, c, d});
}

inline std::array<Vec3, 8> box_points(const Vec3 size) {
  return {
    Vec3(-1, -1, -1) * size, Vec3(1, -1, -1) * size, Vec3(1, -1, 1) * size, Vec3(-1, -1, 1) * size,
    Vec3(-1, 1, -1) * size, Vec3(1, 1, -1) * size, Vec3(1, 1, 1) * size, Vec3(-1, 1, 1) * size};
}

inline std::vector<triangle> make_box(const Vec3 size) {
  const auto p = box_points(size);
  std::vector<triangle> result;
  result.reserve(12);
  quad(result, p[0], p[1], p[2], p[3]);
  quad(result, p[7], p[6], p[5], p[4]);
  quad(result, p[0], p[4], p[5], p[1]);
  quad(result, p[1], p[5], p[6], p[2]);
  quad(result, p[2], p[6], p[7], p[3]);
  quad(result, p[3], p[7], p[4], p[0]);
  return result;
}

inline round_rings round_points(const geometry& shape) {
  const auto radius = shape.size.GetX();
  const auto half_height = shape.size.GetY();
  round_rings rings;
  for (int row = 0; row < int(rings.size()); ++row) {
    const bool upper = row > round_bands;
    const float elevation = (upper ? row - (round_bands + 1) : row - round_bands) * (pi / (2 * round_bands));
    const float y = std::sin(elevation) * radius + (upper ? half_height : -half_height);
    const float ring_radius = std::cos(elevation) * radius;
    for (int i = 0; i < round_sides; ++i) {
      const float azimuth = 2 * pi * i / round_sides;
      rings[size_t(row)][size_t(i)] = Vec3(std::cos(azimuth) * ring_radius, y, std::sin(azimuth) * ring_radius);
    }
  }
  return rings;
}

inline std::vector<triangle> make_round(const geometry& shape) {
  const auto rings = round_points(shape);
  std::vector<triangle> result;
  result.reserve((rings.size() - 1) * round_sides * 2);
  for (size_t row = 1; row < rings.size(); ++row) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; ++i) {
      const auto next = (i + 1) % round_sides;
      const auto a = rings[row - 1][i], b = rings[row][i], c = rings[row][next], d = rings[row - 1][next];
      if (row + 1 != rings.size()) result.push_back({a, b, c});
      if (row != 1) result.push_back({a, c, d});
    }
  }
  return result;
}

inline std::vector<line> make_outline(const geometry& shape) {
  std::vector<line> result;
  if (shape.kind == primitive::box) {
    const auto p = box_points(shape.size);
    result.reserve(12);
    for (size_t i = 0; i < 4; ++i) {
      const auto next = (i + 1) % 4;
      result.push_back({p[i], p[next]});
      result.push_back({p[4 + i], p[4 + next]});
      result.push_back({p[i], p[4 + i]});
    }
    return result;
  }

  const auto rings = round_points(shape);
  result.reserve(4 * round_sides + 4 * (rings.size() - 1));
  for (const auto row : {round_bands / 2, round_bands, round_bands + 1, round_bands + 1 + round_bands / 2}) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; ++i)
      result.push_back({rings[row][i], rings[row][(i + 1) % round_sides]});
  }
  for (size_t row = 1; row < rings.size(); ++row) {
    if (row == round_bands + 1 && shape.kind == primitive::sphere) continue;
    for (size_t i = 0; i < round_sides; i += round_sides / 4)
      result.push_back({rings[row - 1][i], rings[row][i]});
  }
  return result;
}

static_assert(sizeof(vertex) == 32);
static_assert(sizeof(glm::mat4) == 64);

inline glm::vec3 glm_vec(const Vec3 value) {
  return {value.GetX(), value.GetY(), value.GetZ()};
}

inline glm::vec3 tint_from_rgb(const uint32_t rgb) {
  return glm::vec3((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255) / 255.0f;
}

inline void append(std::vector<vertex>& target, const std::vector<triangle>& mesh, const transform& pose,
            const uint32_t rgb, const bool grid = false) {
  const auto tint = tint_from_rgb(rgb);
  const auto light = Vec3(-.43f, .82f, .36f).Normalized();
  const auto center = glm_vec(Vec3(pose.position));
  for (const auto& face : mesh) {
    const auto normal = pose.rotation * (face[1] - face[0]).Cross(face[2] - face[0]).NormalizedOr(Vec3::sAxisY());
    const float lit = .46f + .54f * std::max(0.0f, normal.Dot(light));
    const auto color = glm::vec4(tint * lit, grid && normal.GetY() > .9f ? 1 : 0);
    for (const auto point : face)
      target.push_back({glm::vec4(center + glm_vec(pose.rotation * point), 1), color});
  }
}

inline void append_outline(std::vector<vertex>& target, const std::vector<line>& mesh, const transform& pose,
                    const uint32_t rgb, const float opacity) {
  // The shared shader uses nonnegative alpha for the solid floor grid marker.
  const auto color = glm::vec4(tint_from_rgb(rgb), -opacity);
  const auto center = glm_vec(Vec3(pose.position));
  for (const auto& edge : mesh)
    for (const auto point : edge)
      target.push_back({glm::vec4(center + glm_vec(pose.rotation * point), 1), color});
}

}
