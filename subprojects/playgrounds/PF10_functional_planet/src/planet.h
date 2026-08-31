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
constexpr uint32_t default_mesh_side = 512;
constexpr uint32_t no_region = 0xffffffffu;
constexpr float political_edge_range = 0.04f; // radians retained by the compact GPU distance field

enum class region_kind : uint32_t { land, water, mountain, polar };

struct region_sample {
  uint32_t id = no_region;
  region_kind kind = region_kind::land;
  float edge_distance = 0.0f;
  uint32_t cell_key = no_region; // exact Voronoi feature identity for land/mountain cells
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
  float edge_distance = 0.0f; // approximate angular distance in radians
  uint32_t cell_key = no_region;
};
static_assert(sizeof(political_texel) == 12);

struct province_graph {
  std::vector<uint32_t> province_ids;      // sorted; CSR node index -> stable planet-local id
  std::vector<uint32_t> neighbour_offsets; // size = province_ids.size() + 1
  std::vector<uint32_t> neighbours;        // CSR values are node indices, not hashed ids
  std::vector<glm::vec3> centre_directions;
  std::vector<glm::vec3> label_curve_starts;
  std::vector<glm::vec3> label_directions; // area-centred quadratic Bezier control point
  std::vector<glm::vec3> label_curve_ends;
  std::vector<float> label_clearance;      // approximate angular radius available around label_directions
  std::vector<uint8_t> coastal;
  uint32_t undirected_edges = 0;
  uint32_t connected_components = 0;
};

struct political_atlas {
  uint32_t face_side = 0;
  uint32_t water_regions = 0;
  uint32_t mountain_regions = 0;
  uint32_t polar_regions = 0;
  std::vector<political_texel> texels; // six cube faces, (face_side + 1)^2 nodes each
  province_graph graph;
};

// Runtime rendering does not need a full cell record at every texel. A local R16 index addresses the exact
// Voronoi feature/owner table; the other R16 is a conservative trigger distance for near-field refinement.
struct alignas(16) political_cell_record {
  glm::vec4 feature{}; // query-space feature.xyz; w=1 for Voronoi cells, 0 for analytic water/poles
  glm::uvec4 metadata{}; // stable owner ID, region_kind, reserved, reserved
};
static_assert(sizeof(political_cell_record) == 32);

struct packed_political_atlas {
  uint32_t face_side = 0;
  std::vector<uint32_t> texels;
  std::vector<political_cell_record> cells;
};

struct alignas(16) surface_patch {
  uint32_t face = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t pad = 0;
};
static_assert(sizeof(surface_patch) == 16);

// Independent surface-feature layer: rivers are tapered spherical capsules, lakes are filled spherical discs.
// They do not consume province IDs and therefore cannot disturb the navigation graph.
struct alignas(16) hydrology_feature {
  glm::vec4 a_direction_height{};
  glm::vec4 b_direction_height{};
  glm::vec4 widths_kind{}; // endpoint half-widths in radians, kind (0 river / 1 lake), colour variant
};
static_assert(sizeof(hydrology_feature) == 48);

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
glm::vec3 orbit_camera_direction(glm::vec3 direction, float horizontal, float vertical,
                                 float angular_step) noexcept;
std::vector<glm::vec4> bake_surface_vertices(uint32_t face_side);
political_atlas bake_political_atlas(uint32_t face_side);
packed_political_atlas pack_political_atlas(const political_atlas& source);
std::vector<surface_patch> visible_surface_patches(uint32_t face_side, uint32_t patch_side,
                                                   glm::vec3 local_eye);
std::vector<surface_patch> refined_surface_patches(uint32_t face_side, uint32_t patch_side,
                                                   glm::vec3 local_eye);
std::vector<hydrology_feature> make_hydrology_features();
surface_hit intersect_surface(glm::vec3 ray_origin, glm::vec3 ray_direction) noexcept;
std::vector<landmark> make_landmarks(uint32_t count);

struct survey_result {
  uint32_t land_regions = 0;
  uint32_t water_regions = 0;
  uint32_t mountain_regions = 0;
  uint32_t polar_regions = 0;
  float sampled_min_height = 0.0f;
  float sampled_max_height = 0.0f;
  uint64_t fingerprint = 0;
};

survey_result survey_planet(uint32_t samples);
const char* region_kind_name(region_kind kind) noexcept;

} // namespace devils_engine::pf10
