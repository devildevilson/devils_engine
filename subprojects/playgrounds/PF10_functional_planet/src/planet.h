#pragma once

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace devils_engine::pf10 {

constexpr float planet_radius = 1.0f;
constexpr float minimum_height = -0.045f;
constexpr float maximum_height = 0.085f;
constexpr float province_frequency = 25.0f;
constexpr uint32_t default_mesh_side = 256;
constexpr uint32_t no_region = 0xffffffffu;

enum class region_kind : uint32_t { land, water, polar };

struct region_sample {
  uint32_t id = no_region;
  region_kind kind = region_kind::land;
  float edge_distance = 0.0f;
};

struct surface_hit {
  bool hit = false;
  float distance = 0.0f;
  glm::vec3 direction{0.0f, 1.0f, 0.0f};
  float height = 0.0f;
  region_sample region{};
};

enum class landmark_kind : uint32_t { city, wonder, construction };

// GPU-facing record. direction_height.xyz is planet-local and survives every globe transform.
struct alignas(16) landmark {
  glm::vec4 direction_height{};
  glm::vec4 colour_scale{};
  uint32_t region_id = no_region;
  uint32_t kind = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;
};
static_assert(sizeof(landmark) == 48);

uint32_t hash_cell(glm::ivec3 cell) noexcept;
float surface_height(glm::vec3 direction) noexcept;
region_sample sample_region(glm::vec3 direction) noexcept;
glm::vec3 surface_position(glm::vec3 direction) noexcept;
surface_hit intersect_surface(glm::vec3 ray_origin, glm::vec3 ray_direction) noexcept;
std::vector<landmark> make_landmarks(uint32_t count);

struct survey_result {
  uint32_t land_regions = 0;
  uint32_t water_regions = 0;
  uint32_t polar_regions = 0;
  float sampled_min_height = 0.0f;
  float sampled_max_height = 0.0f;
  uint64_t fingerprint = 0;
};

survey_result survey_planet(uint32_t samples);
const char* region_kind_name(region_kind kind) noexcept;

} // namespace devils_engine::pf10
