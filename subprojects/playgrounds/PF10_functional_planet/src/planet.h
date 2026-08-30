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

// Compact static political field used by the fragment shader.  The procedural generator remains the
// canonical authoring/picking source; this is its one-time runtime bake.
struct political_texel {
  uint32_t region_id = no_region;
  float edge_distance = 0.0f;
};
static_assert(sizeof(political_texel) == 8);

struct province_graph {
  std::vector<uint32_t> province_ids;      // sorted; CSR node index -> stable planet-local id
  std::vector<uint32_t> neighbour_offsets; // size = province_ids.size() + 1
  std::vector<uint32_t> neighbours;        // CSR values are node indices, not hashed ids
  std::vector<glm::vec3> centre_directions;
  std::vector<glm::vec3> label_directions; // deepest sampled interior point, not merely the centroid
  std::vector<float> label_clearance;      // approximate angular radius available around label_directions
  std::vector<uint8_t> coastal;
  uint32_t undirected_edges = 0;
  uint32_t connected_components = 0;
};

struct political_atlas {
  uint32_t face_side = 0;
  uint32_t water_regions = 0;
  uint32_t polar_regions = 0;
  std::vector<political_texel> texels; // six cube faces, (face_side + 1)^2 nodes each
  province_graph graph;
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
std::vector<glm::vec4> bake_surface_vertices(uint32_t face_side);
political_atlas bake_political_atlas(uint32_t face_side);
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
