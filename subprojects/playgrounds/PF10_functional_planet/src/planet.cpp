#include "planet.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

namespace devils_engine::pf10 {
namespace {

constexpr uint32_t water_bit = 0x80000000u;
constexpr uint32_t polar_bit = 0xc0000000u;

uint32_t mix32(uint32_t value) noexcept {
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

float hash01(const glm::ivec3 cell, const uint32_t salt) noexcept {
  return float((mix32(hash_cell(cell) ^ salt) >> 8u) & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float value_noise(const glm::vec3 point) noexcept {
  const glm::ivec3 base = glm::ivec3(glm::floor(point));
  glm::vec3 blend = glm::fract(point);
  blend = blend * blend * (glm::vec3(3.0f) - 2.0f * blend);

  std::array<float, 8> values{};
  for (int z = 0; z < 2; ++z) {
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        values[size_t(x + y * 2 + z * 4)] = hash01(base + glm::ivec3(x, y, z), 0x68bc21ebu);
      }
    }
  }
  const float x00 = glm::mix(values[0], values[1], blend.x);
  const float x10 = glm::mix(values[2], values[3], blend.x);
  const float x01 = glm::mix(values[4], values[5], blend.x);
  const float x11 = glm::mix(values[6], values[7], blend.x);
  return glm::mix(glm::mix(x00, x10, blend.y), glm::mix(x01, x11, blend.y), blend.z);
}

float fractal_noise(glm::vec3 point) noexcept {
  float value = 0.0f;
  float weight = 0.5333333f;
  for (uint32_t octave = 0; octave < 4; ++octave) {
    value += value_noise(point) * weight;
    point = point * 2.031f + glm::vec3(7.13f, 3.71f, 11.17f);
    weight *= 0.5f;
  }
  return value;
}

struct classification {
  region_kind kind = region_kind::land;
  uint32_t id = no_region;
  float edge = std::numeric_limits<float>::max();
};

struct ocean_query {
  float best = -100.0f;
  float second = -100.0f;
  uint32_t index = 0;
};

ocean_query sample_oceans(const glm::vec3 direction) noexcept {
  constexpr std::array<glm::vec4, 4> oceans{
    glm::vec4{0.781f, 0.120f, 0.613f, 0.675f}, glm::vec4{-0.704f, 0.151f, 0.694f, 0.715f},
    glm::vec4{0.050f, -0.249f, -0.967f, 0.700f}, glm::vec4{-0.252f, 0.504f, -0.826f, 0.775f}};

  ocean_query result{};
  const float coast_noise = (value_noise(direction * 3.7f + glm::vec3(17.0f, 5.0f, 29.0f)) - 0.5f) * 0.085f;
  for (uint32_t i = 0; i < oceans.size(); ++i) {
    const float score = glm::dot(direction, glm::vec3(oceans[i])) - oceans[i].w + coast_noise;
    if (score > result.best) {
      result.second = result.best;
      result.best = score;
      result.index = i;
    } else if (score > result.second) {
      result.second = score;
    }
  }
  return result;
}

classification classify_non_land(const glm::vec3 direction) noexcept {
  const float polar_edge = std::abs(std::abs(direction.y) - 0.91f);
  if (std::abs(direction.y) >= 0.91f) {
    return {region_kind::polar, polar_bit | (direction.y >= 0.0f ? 1u : 2u), polar_edge};
  }

  // Four intentionally broad temporary oceans. Their exact geography is content, not a PF10 mechanism;
  // region kind and stable identity are the contract that later surface generation will feed.
  const auto ocean = sample_oceans(direction);
  if (ocean.best > 0.0f) {
    return {region_kind::water, water_bit | (ocean.index + 1u), std::min(ocean.best, ocean.best - ocean.second)};
  }
  return {region_kind::land, no_region, std::min(-ocean.best, polar_edge)};
}

glm::vec3 fibonacci_direction(const uint32_t index, const uint32_t count) noexcept {
  constexpr float golden_angle = 2.39996322972865332f;
  const float y = 1.0f - 2.0f * (float(index) + 0.5f) / float(count);
  const float radius = std::sqrt(std::max(1.0f - y * y, 0.0f));
  const float angle = float(index) * golden_angle;
  return {std::cos(angle) * radius, y, std::sin(angle) * radius};
}

glm::vec3 cube_direction(const uint32_t face, const glm::vec2 uv) noexcept {
  if (face == 0u) return glm::normalize(glm::vec3(1.0f, uv.y, -uv.x));
  if (face == 1u) return glm::normalize(glm::vec3(-1.0f, uv.y, uv.x));
  if (face == 2u) return glm::normalize(glm::vec3(uv.x, 1.0f, -uv.y));
  if (face == 3u) return glm::normalize(glm::vec3(uv.x, -1.0f, uv.y));
  if (face == 4u) return glm::normalize(glm::vec3(uv.x, uv.y, 1.0f));
  return glm::normalize(glm::vec3(-uv.x, uv.y, -1.0f));
}

bool is_land_id(const uint32_t id) noexcept { return (id & 0xc0000000u) == 0u; }

} // namespace

uint32_t hash_cell(const glm::ivec3 cell) noexcept {
  uint32_t value = uint32_t(cell.x) * 0x8da6b343u;
  value ^= uint32_t(cell.y) * 0xd8163841u;
  value ^= uint32_t(cell.z) * 0xcb1ab31fu;
  return mix32(value);
}

float surface_height(glm::vec3 direction) noexcept {
  direction = glm::normalize(direction);
  const float continent = fractal_noise(direction * 1.60f + glm::vec3(3.0f, 9.0f, 1.0f));
  const float folds = fractal_noise(direction * 4.20f + glm::vec3(31.0f, 7.0f, 19.0f));
  const float detail = value_noise(direction * 18.0f + glm::vec3(13.0f, 37.0f, 5.0f)) - 0.5f;
  const float ridge = 1.0f - std::abs(folds * 2.0f - 1.0f);
  const float mountain_mask = glm::smoothstep(0.43f, 0.61f, continent);
  float height = (continent - 0.50f) * 0.11f + std::pow(ridge, 4.0f) * 0.032f * mountain_mask +
                 detail * 0.008f - 0.006f;
  // There is still one displaced surface in slice 1. A broad water navigation region therefore owns a
  // nearly level radial shell, while coast terrain rises out of it; a separate reflective ocean comes later.
  if (std::abs(direction.y) < 0.91f) {
    const float coast_blend = glm::smoothstep(-0.055f, 0.025f, sample_oceans(direction).best);
    height = glm::mix(height, -0.018f + detail * 0.0015f, coast_blend);
  }
  return std::clamp(height, minimum_height, maximum_height);
}

region_sample sample_region(glm::vec3 direction) noexcept {
  direction = glm::normalize(direction);
  const auto non_land = classify_non_land(direction);
  if (non_land.kind != region_kind::land) return {non_land.id, non_land.kind, non_land.edge};

  const glm::vec3 query = direction * province_frequency;
  const glm::ivec3 base = glm::ivec3(glm::floor(query));
  float nearest = std::numeric_limits<float>::max();
  float second = std::numeric_limits<float>::max();
  uint32_t nearest_id = no_region;
  for (int z = -1; z <= 1; ++z) {
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        const glm::ivec3 cell = base + glm::ivec3(x, y, z);
        const glm::vec3 feature = glm::vec3(cell) + glm::vec3(hash01(cell, 0xa511e9b3u),
                                                              hash01(cell, 0x63d83595u),
                                                              hash01(cell, 0xb5297a4du));
        const glm::vec3 delta = query - feature;
        const float distance = glm::dot(delta, delta);
        if (distance < nearest) {
          second = nearest;
          nearest = distance;
          nearest_id = (hash_cell(cell) & 0x3fffffffu) | 1u;
        } else if (distance < second) {
          second = distance;
        }
      }
    }
  }
  const float voronoi_edge = std::sqrt(second) - std::sqrt(nearest);
  return {nearest_id, region_kind::land, std::min(voronoi_edge, non_land.edge * province_frequency)};
}

glm::vec3 surface_position(glm::vec3 direction) noexcept {
  direction = glm::normalize(direction);
  return direction * (planet_radius + surface_height(direction));
}

std::vector<glm::vec4> bake_surface_vertices(const uint32_t face_side) {
  const uint32_t side = std::max(face_side, 1u);
  const uint64_t nodes_per_face = uint64_t(side + 1u) * uint64_t(side + 1u);
  std::vector<glm::vec4> result(size_t(nodes_per_face * 6u));
  for (uint32_t face = 0; face < 6u; ++face) {
    for (uint32_t y = 0; y <= side; ++y) {
      for (uint32_t x = 0; x <= side; ++x) {
        const glm::vec2 uv = glm::vec2(float(x), float(y)) / float(side) * 2.0f - 1.0f;
        const glm::vec3 direction = cube_direction(face, uv);
        result[size_t(uint64_t(face) * nodes_per_face + uint64_t(y) * (side + 1u) + x)] =
          glm::vec4(surface_position(direction), 0.0f);
      }
    }
  }
  return result;
}

political_atlas bake_political_atlas(const uint32_t face_side) {
  political_atlas result{};
  result.face_side = std::max(face_side, 1u);
  const uint32_t side = result.face_side;
  const uint64_t nodes_per_face = uint64_t(side + 1u) * uint64_t(side + 1u);
  result.texels.resize(size_t(nodes_per_face * 6u));

  struct centre_accumulator {
    glm::vec3 sum{0.0f};
    glm::vec3 label_direction{0.0f, 1.0f, 0.0f};
    float best_edge = -1.0f;
    uint32_t count = 0;
    bool coastal = false;
  };
  std::unordered_map<uint32_t, centre_accumulator> centres;
  centres.reserve(5000);
  std::unordered_set<uint32_t> water_ids;
  std::unordered_set<uint32_t> polar_ids;
  std::unordered_set<uint64_t> edge_set;
  edge_set.reserve(16000);

  const auto texel_index = [=](const uint32_t face, const uint32_t x, const uint32_t y) {
    return size_t(uint64_t(face) * nodes_per_face + uint64_t(y) * (side + 1u) + x);
  };
  for (uint32_t face = 0; face < 6u; ++face) {
    for (uint32_t y = 0; y <= side; ++y) {
      for (uint32_t x = 0; x <= side; ++x) {
        const glm::vec2 uv = glm::vec2(float(x), float(y)) / float(side) * 2.0f - 1.0f;
        const glm::vec3 direction = cube_direction(face, uv);
        const auto region = sample_region(direction);
        result.texels[texel_index(face, x, y)] = {region.id, region.edge_distance};
        if (region.kind == region_kind::land) {
          auto& centre = centres[region.id];
          centre.sum += direction;
          ++centre.count;
          if (region.edge_distance > centre.best_edge) {
            centre.best_edge = region.edge_distance;
            centre.label_direction = direction;
          }
        } else if (region.kind == region_kind::water) water_ids.insert(region.id);
        else polar_ids.insert(region.id);
      }
    }
  }

  const auto join = [&](const political_texel a, const political_texel b) {
    if (a.region_id == b.region_id) return;
    const bool a_land = is_land_id(a.region_id);
    const bool b_land = is_land_id(b.region_id);
    if (a_land && b_land) {
      const uint32_t low = std::min(a.region_id, b.region_id);
      const uint32_t high = std::max(a.region_id, b.region_id);
      edge_set.insert((uint64_t(low) << 32u) | uint64_t(high));
    } else {
      if (a_land) centres[a.region_id].coastal = true;
      if (b_land) centres[b.region_id].coastal = true;
    }
  };
  // Shared cube edges are sampled by both faces.  Their identical seam node plus each face's first inward
  // node makes cross-face province contacts appear in these ordinary horizontal/vertical comparisons too.
  for (uint32_t face = 0; face < 6u; ++face) {
    for (uint32_t y = 0; y <= side; ++y) {
      for (uint32_t x = 0; x <= side; ++x) {
        const auto current = result.texels[texel_index(face, x, y)];
        if (x < side) join(current, result.texels[texel_index(face, x + 1u, y)]);
        if (y < side) join(current, result.texels[texel_index(face, x, y + 1u)]);
      }
    }
  }

  auto& graph = result.graph;
  result.water_regions = uint32_t(water_ids.size());
  result.polar_regions = uint32_t(polar_ids.size());
  graph.province_ids.reserve(centres.size());
  for (const auto& [id, _] : centres) graph.province_ids.push_back(id);
  std::ranges::sort(graph.province_ids);
  std::unordered_map<uint32_t, uint32_t> node_index;
  node_index.reserve(graph.province_ids.size());
  graph.centre_directions.resize(graph.province_ids.size());
  graph.label_directions.resize(graph.province_ids.size());
  graph.label_clearance.resize(graph.province_ids.size());
  graph.coastal.resize(graph.province_ids.size());
  for (uint32_t i = 0; i < graph.province_ids.size(); ++i) {
    const uint32_t id = graph.province_ids[i];
    node_index.emplace(id, i);
    const auto& centre = centres.at(id);
    graph.centre_directions[i] = glm::normalize(centre.sum);
    graph.label_directions[i] = centre.label_direction;
    graph.label_clearance[i] = centre.best_edge / province_frequency;
    graph.coastal[i] = uint8_t(centre.coastal);
  }

  std::vector<std::pair<uint32_t, uint32_t>> edges;
  edges.reserve(edge_set.size());
  for (const uint64_t packed : edge_set) {
    edges.emplace_back(node_index.at(uint32_t(packed >> 32u)), node_index.at(uint32_t(packed)));
  }
  std::ranges::sort(edges);
  graph.undirected_edges = uint32_t(edges.size());
  graph.neighbour_offsets.assign(graph.province_ids.size() + 1u, 0u);
  for (const auto [a, b] : edges) {
    ++graph.neighbour_offsets[a + 1u];
    ++graph.neighbour_offsets[b + 1u];
  }
  for (uint32_t i = 1; i < graph.neighbour_offsets.size(); ++i) {
    graph.neighbour_offsets[i] += graph.neighbour_offsets[i - 1u];
  }
  graph.neighbours.resize(edges.size() * 2u);
  std::vector<uint32_t> cursors = graph.neighbour_offsets;
  for (const auto [a, b] : edges) {
    graph.neighbours[cursors[a]++] = b;
    graph.neighbours[cursors[b]++] = a;
  }
  for (uint32_t node = 0; node < graph.province_ids.size(); ++node) {
    std::sort(graph.neighbours.begin() + graph.neighbour_offsets[node],
              graph.neighbours.begin() + graph.neighbour_offsets[node + 1u]);
  }

  std::vector<uint8_t> visited(graph.province_ids.size(), 0u);
  std::vector<uint32_t> pending;
  pending.reserve(graph.province_ids.size());
  for (uint32_t root = 0; root < graph.province_ids.size(); ++root) {
    if (visited[root]) continue;
    ++graph.connected_components;
    visited[root] = 1u;
    pending.push_back(root);
    while (!pending.empty()) {
      const uint32_t node = pending.back();
      pending.pop_back();
      for (uint32_t i = graph.neighbour_offsets[node]; i < graph.neighbour_offsets[node + 1u]; ++i) {
        const uint32_t neighbour = graph.neighbours[i];
        if (!visited[neighbour]) {
          visited[neighbour] = 1u;
          pending.push_back(neighbour);
        }
      }
    }
  }
  return result;
}

surface_hit intersect_surface(const glm::vec3 ray_origin, glm::vec3 ray_direction) noexcept {
  ray_direction = glm::normalize(ray_direction);
  const float outer = planet_radius + maximum_height;
  const float b = glm::dot(ray_origin, ray_direction);
  const float c = glm::dot(ray_origin, ray_origin) - outer * outer;
  const float discriminant = b * b - c;
  if (discriminant < 0.0f) return {};

  const float root = std::sqrt(discriminant);
  const float begin = std::max(-b - root, 0.0f);
  const float end = -b + root;
  if (end <= begin) return {};

  auto signed_distance = [&](const float t) {
    const glm::vec3 point = ray_origin + ray_direction * t;
    return glm::length(point) - (planet_radius + surface_height(point));
  };

  constexpr uint32_t march_steps = 128;
  float previous_t = begin;
  float previous_value = signed_distance(previous_t);
  for (uint32_t i = 1; i <= march_steps; ++i) {
    const float t = glm::mix(begin, end, float(i) / float(march_steps));
    const float value = signed_distance(t);
    if (value <= 0.0f && previous_value >= 0.0f) {
      float low = previous_t;
      float high = t;
      for (uint32_t iteration = 0; iteration < 18; ++iteration) {
        const float middle = (low + high) * 0.5f;
        if (signed_distance(middle) > 0.0f) low = middle;
        else high = middle;
      }
      const float hit_t = (low + high) * 0.5f;
      const glm::vec3 direction = glm::normalize(ray_origin + ray_direction * hit_t);
      return {true, hit_t, direction, surface_height(direction), sample_region(direction)};
    }
    previous_t = t;
    previous_value = value;
  }
  return {};
}

std::vector<landmark> make_landmarks(const uint32_t count) {
  std::vector<landmark> result;
  result.reserve(count);
  for (uint32_t candidate = 0; result.size() < count && candidate < count * 128u; ++candidate) {
    const glm::vec3 direction = fibonacci_direction(candidate * 17u + 11u, count * 128u + 31u);
    const auto region = sample_region(direction);
    if (region.kind != region_kind::land) continue;

    const auto kind = landmark_kind(result.size() % 3u);
    const glm::vec3 colour = kind == landmark_kind::city          ? glm::vec3(0.96f, 0.77f, 0.24f)
                             : kind == landmark_kind::wonder      ? glm::vec3(0.87f, 0.32f, 0.83f)
                                                                  : glm::vec3(0.32f, 0.88f, 0.94f);
    const float scale = kind == landmark_kind::wonder ? 1.35f : (kind == landmark_kind::construction ? 1.15f : 1.0f);
    result.push_back({glm::vec4(direction, surface_height(direction)), glm::vec4(colour, scale), region.id,
                      uint32_t(kind), 0, 0});
  }
  return result;
}

survey_result survey_planet(const uint32_t samples) {
  survey_result result{};
  result.sampled_min_height = std::numeric_limits<float>::max();
  result.sampled_max_height = std::numeric_limits<float>::lowest();
  std::unordered_set<uint32_t> land;
  std::unordered_set<uint32_t> water;
  std::unordered_set<uint32_t> polar;
  land.reserve(6000);

  uint64_t fingerprint = 0xcbf29ce484222325ull;
  for (uint32_t i = 0; i < samples; ++i) {
    const glm::vec3 direction = fibonacci_direction(i, samples);
    const float height = surface_height(direction);
    const auto region = sample_region(direction);
    result.sampled_min_height = std::min(result.sampled_min_height, height);
    result.sampled_max_height = std::max(result.sampled_max_height, height);
    if (region.kind == region_kind::land) land.insert(region.id);
    else if (region.kind == region_kind::water) water.insert(region.id);
    else polar.insert(region.id);

    if ((i & 63u) == 0u) {
      fingerprint ^= uint64_t(region.id) | (uint64_t(std::bit_cast<uint32_t>(height)) << 32u);
      fingerprint *= 0x100000001b3ull;
    }
  }
  result.land_regions = uint32_t(land.size());
  result.water_regions = uint32_t(water.size());
  result.polar_regions = uint32_t(polar.size());
  result.fingerprint = fingerprint;
  return result;
}

const char* region_kind_name(const region_kind kind) noexcept {
  switch (kind) {
    case region_kind::land: return "land province";
    case region_kind::water: return "water region";
    case region_kind::polar: return "non-playable polar region";
  }
  return "unknown";
}

} // namespace devils_engine::pf10
