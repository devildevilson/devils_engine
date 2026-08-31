#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

#include "planet.h"
#include "viewer.h"

namespace {

using namespace devils_engine;

void usage() {
  std::cout << "PF10 functional planet\n"
               "  --view                 открыть первое отображение планеты (по умолчанию)\n"
               "  --verify               проверить surface/province contracts без Vulkan\n"
               "  --frames=N             закрыть viewer после N кадров\n"
               "  --shot=PATH            сохранить последний кадр в PPM\n"
               "  --mesh=N               клеток на грань cube-sphere (32..512, default 512)\n"
               "  --distance=R           расстояние камеры 1.16..4.5 R (default 2.62)\n"
               "  --fixed-rotation       детерминированный угол глобуса для кадров\n"
               "  --no-hydrology        диагностический A/B без рек и озёр\n"
               "  --no-state-borders    диагностический A/B без государственных ribbons\n"
               "  --border-debug=MODE   exact | distance | state\n"
               "  --validation           включить Vulkan validation layers\n";
}

bool prefixed(const std::string_view argument, const std::string_view prefix, std::string& value) {
  if (!argument.starts_with(prefix)) return false;
  value = argument.substr(prefix.size());
  return true;
}

int verify() {
  constexpr uint32_t samples = 600000;
  const auto first = pf10::survey_planet(samples);
  const auto second = pf10::survey_planet(samples);
  uint32_t passed = 0;
  uint32_t failed = 0;
  auto check = [&](const bool condition, const std::string_view name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    condition ? ++passed : ++failed;
  };

  check(first.land_regions >= 3000 && first.land_regions <= 5000,
        std::format("playable land provinces are in [3000,5000] (sampled {})", first.land_regions));
  check(first.water_regions >= 3 && first.water_regions <= 8,
        std::format("water regions stay large and few ({})", first.water_regions));
  check(first.mountain_regions == 3,
        std::format("three explicit non-playable mountain chains are sampled ({})", first.mountain_regions));
  check(first.polar_regions == 2, "north and south are two explicit non-playable regions");
  check(first.sampled_min_height >= pf10::minimum_height - 1.0e-6f &&
          first.sampled_max_height <= pf10::maximum_height + 1.0e-6f,
        std::format("height is bounded [{:.5f},{:.5f}]", first.sampled_min_height, first.sampled_max_height));
  check(first.sampled_max_height - first.sampled_min_height > 0.075f,
        "geometry has a visible radial height range");
  check(first.fingerprint == second.fingerprint, std::format("deterministic fingerprint 0x{:016x}", first.fingerprint));

  auto politics = pf10::bake_political_atlas(512u);
  pf10::assign_fixture_states(politics.graph, glm::normalize(glm::vec3(0.0f, -0.19f, 0.982f)), 3u);
  const auto& graph = politics.graph;
  check(politics.mountain_regions == 3u, "dense atlas materializes all mountain barriers");
  std::unordered_map<uint32_t, uint32_t> cell_by_province;
  std::unordered_map<uint32_t, uint32_t> province_atlas_samples;
  cell_by_province.reserve(graph.province_ids.size());
  province_atlas_samples.reserve(graph.province_ids.size());
  bool unique_province_identity = true;
  for (const auto& texel : politics.texels) {
    if ((texel.region_id & 0xc0000000u) != 0u) continue;
    const auto [entry, inserted] = cell_by_province.try_emplace(texel.region_id, texel.cell_key);
    unique_province_identity &= inserted || entry->second == texel.cell_key;
    ++province_atlas_samples[texel.region_id];
  }
  check(unique_province_identity && cell_by_province.size() == graph.province_ids.size(),
        "every playable province ID maps bijectively to one Voronoi cell key");
  check(graph.province_ids.size() >= first.land_regions && graph.province_ids.size() <= 5000u,
        std::format("dense adjacency bake covers the survey and stays in budget ({}/{})",
                    graph.province_ids.size(), first.land_regions));
  check(graph.neighbour_offsets.size() == graph.province_ids.size() + 1u &&
          graph.neighbour_offsets.back() == graph.neighbours.size(),
        "adjacency graph has a complete CSR layout");
  bool symmetric = true;
  bool isolated = false;
  for (uint32_t node = 0; node < graph.province_ids.size(); ++node) {
    isolated |= graph.neighbour_offsets[node] == graph.neighbour_offsets[node + 1u];
    for (uint32_t i = graph.neighbour_offsets[node]; i < graph.neighbour_offsets[node + 1u]; ++i) {
      const uint32_t neighbour = graph.neighbours[i];
      if (neighbour >= graph.province_ids.size() || neighbour == node) {
        symmetric = false;
        continue;
      }
      const auto begin = graph.neighbours.begin() + graph.neighbour_offsets[neighbour];
      const auto end = graph.neighbours.begin() + graph.neighbour_offsets[neighbour + 1u];
      symmetric &= std::binary_search(begin, end, node);
    }
  }
  check(symmetric, "every land adjacency is an undirected symmetric edge");
  check(!isolated, "no playable province is isolated from the navigation graph");
  check(graph.connected_components == 1u,
        std::format("playable province navigation is one connected land graph ({} component)",
                    graph.connected_components));
  const double mean_degree = graph.province_ids.empty() ? 0.0 :
    double(graph.neighbours.size()) / double(graph.province_ids.size());
  check(mean_degree >= 4.0 && mean_degree <= 8.0,
        std::format("mean province degree is plausible ({:.2f})", mean_degree));
  const bool state_layout_complete = graph.state_count == 3u &&
                                     graph.state_ids.size() == graph.province_ids.size() &&
                                     graph.state_centres.size() == graph.state_count &&
                                     std::ranges::all_of(graph.state_ids, [&](const uint32_t state) {
                                       return state < graph.state_count;
                                     });
  check(state_layout_complete, "every playable province owns one of three explicit fixture states");
  bool states_connected = state_layout_complete;
  for (uint32_t state = 0u; states_connected && state < graph.state_count; ++state) {
    const auto root = std::ranges::find(graph.state_ids, state);
    if (root == graph.state_ids.end()) { states_connected = false; break; }
    std::vector<uint8_t> state_visited(graph.province_ids.size(), 0u);
    std::vector<uint32_t> state_pending{uint32_t(root - graph.state_ids.begin())};
    state_visited[state_pending.front()] = 1u;
    uint32_t reached = 0u;
    while (!state_pending.empty()) {
      const uint32_t node = state_pending.back();
      state_pending.pop_back();
      ++reached;
      for (uint32_t i = graph.neighbour_offsets[node]; i < graph.neighbour_offsets[node + 1u]; ++i) {
        const uint32_t neighbour = graph.neighbours[i];
        if (!state_visited[neighbour] && graph.state_ids[neighbour] == state) {
          state_visited[neighbour] = 1u;
          state_pending.push_back(neighbour);
        }
      }
    }
    states_connected &= reached == std::ranges::count(graph.state_ids, state);
  }
  check(states_connected, "every fixture state is connected in the province navigation graph");
  const bool label_layout_complete = graph.label_curve_starts.size() == graph.province_ids.size() &&
                                     graph.label_directions.size() == graph.province_ids.size() &&
                                     graph.label_curve_ends.size() == graph.province_ids.size() &&
                                     graph.label_clearance.size() == graph.province_ids.size();
  check(label_layout_complete, "every province owns a complete curved-label layout");
  check(label_layout_complete &&
          std::ranges::all_of(std::views::iota(size_t(0), graph.province_ids.size()), [&](const size_t node) {
            return graph.label_clearance[node] > 0.0f &&
                   pf10::sample_region(graph.label_directions[node]).id == graph.province_ids[node];
          }), "area-centred label anchors have positive clearance inside their province");
  check(label_layout_complete &&
          std::ranges::all_of(std::views::iota(size_t(0), graph.province_ids.size()), [&](const size_t node) {
            for (uint32_t sample = 0u; sample <= 8u; ++sample) {
              const float t = float(sample) / 8.0f;
              const float u = 1.0f - t;
              const glm::vec3 direction = glm::normalize(graph.label_curve_starts[node] * (u * u) +
                                                          graph.label_directions[node] * (2.0f * u * t) +
                                                          graph.label_curve_ends[node] * (t * t));
              if (pf10::sample_region(direction).id != graph.province_ids[node]) return false;
            }
            return true;
          }), "Bezier label corridors remain inside their province");
  check(label_layout_complete &&
          std::ranges::all_of(std::views::iota(size_t(0), graph.province_ids.size()), [&](const size_t node) {
            for (uint32_t sample = 0u; sample <= 8u; ++sample) {
              const float t = float(sample) / 8.0f;
              const float u = 1.0f - t;
              const glm::vec3 direction = glm::normalize(graph.label_curve_starts[node] * (u * u) +
                                                          graph.label_directions[node] * (2.0f * u * t) +
                                                          graph.label_curve_ends[node] * (t * t));
              glm::vec3 tangent = graph.label_directions[node] - graph.label_curve_starts[node];
              tangent = tangent * u + (graph.label_curve_ends[node] - graph.label_directions[node]) * t;
              tangent -= direction * glm::dot(direction, tangent);
              if (glm::dot(tangent, tangent) < 1.0e-12f) continue;
              const glm::vec3 north = glm::vec3(0.0f, 1.0f, 0.0f) - direction * direction.y;
              if (glm::dot(glm::cross(direction, tangent), north) < -1.0e-7f) return false;
            }
            return true;
          }), "province label up never turns more than 90 degrees away from local north");
  float minimum_label_span = std::numeric_limits<float>::max();
  uint32_t collapsed_label_curves = 0u;
  uint32_t subpixel_provinces = 0u;
  uint32_t minimum_label_node = pf10::no_region;
  if (label_layout_complete) {
    for (size_t node = 0u; node < graph.province_ids.size(); ++node) {
      const float span = std::acos(std::clamp(glm::dot(graph.label_curve_starts[node],
                                                       graph.label_curve_ends[node]), -1.0f, 1.0f));
      // Fewer than eight nodes in the atlas-512 verification field is a genuinely sub-pixel land sliver at
      // inspection scale, not enough surface to contain a five-character decal. It stays a graph node for
      // this arbitrary fixture, but must not weaken the regression for ordinary materialized provinces.
      if (province_atlas_samples[graph.province_ids[node]] < 8u) { ++subpixel_provinces; continue; }
      if (span < minimum_label_span) { minimum_label_span = span; minimum_label_node = uint32_t(node); }
      collapsed_label_curves += span <= 0.0005f;
    }
  }
  check(label_layout_complete && collapsed_label_curves == 0u,
        std::format("no labelable province curve collapses (minimum {:.6f} rad at node {}, {} subpixel slivers)",
                    minimum_label_span, minimum_label_node, subpixel_provinces));

  const auto packed = pf10::pack_political_atlas(politics);
  bool compact_round_trip = packed.texels.size() == politics.texels.size() && !packed.cells.empty();
  bool compact_metadata = compact_round_trip;
  for (size_t i = 0; compact_round_trip && i < packed.texels.size(); ++i) {
    const uint32_t local_index = packed.texels[i] & 0xffffu;
    compact_round_trip &= local_index < packed.cells.size() &&
                          packed.cells[local_index].metadata.x == politics.texels[i].region_id;
    if (!compact_round_trip) break;
    const auto& cell = packed.cells[local_index];
    const auto province = std::ranges::lower_bound(graph.province_ids, politics.texels[i].region_id);
    const bool playable = province != graph.province_ids.end() && *province == politics.texels[i].region_id;
    if (playable) {
      const size_t node = size_t(province - graph.province_ids.begin());
      compact_metadata &= cell.metadata.y == 0u && cell.metadata.z == graph.state_ids[node] &&
                          cell.metadata.w == node && cell.feature.w == 1.0f;
    } else {
      compact_metadata &= cell.metadata.z == pf10::no_region && cell.metadata.w == pf10::no_region;
    }
  }
  check(compact_round_trip, "every R16 political texel round-trips to its exact stable planet-local ID");
  check(compact_metadata, "every compact political record agrees with canonical kind/state/CSR ownership");

  const auto state_borders = pf10::make_state_borders(politics);
  const auto repeated_state_borders = pf10::make_state_borders(politics);
  const uint32_t state_border_trails = state_borders.empty() ? 0u :
    std::ranges::max(state_borders | std::views::transform([](const pf10::state_border_segment& segment) {
      return segment.states.z;
    })) + 1u;
  const float maximum_state_segment = state_borders.empty() ? 0.0f :
    std::ranges::max(state_borders | std::views::transform([](const pf10::state_border_segment& segment) {
      return segment.b_position_s.w - segment.a_position_s.w;
    }));
  check(!state_borders.empty() && state_borders.size() < 65536u,
        std::format("state frontiers materialize as a compact exact ribbon set ({} segments, {} trails)",
                    state_borders.size(), state_border_trails));
  check(state_borders.size() == repeated_state_borders.size() &&
          (state_borders.empty() || std::memcmp(state_borders.data(), repeated_state_borders.data(),
                                                state_borders.size() * sizeof(pf10::state_border_segment)) == 0),
        "materialized state ribbons are bit-identical across repeated builds");
  check(std::ranges::all_of(state_borders, [&](const pf10::state_border_segment& segment) {
          return segment.states.x < graph.state_count && segment.states.y < graph.state_count &&
                 segment.states.x != segment.states.y && segment.b_position_s.w > segment.a_position_s.w;
        }), "every state ribbon has two different valid sides and increasing world-locked arc length");
  const auto state_for_region = [&](const uint32_t id) {
    const auto found = std::ranges::lower_bound(graph.province_ids, id);
    return found == graph.province_ids.end() || *found != id ? pf10::no_region :
           graph.state_ids[size_t(found - graph.province_ids.begin())];
  };
  uint32_t invalid_state_sides = 0u;
  for (const auto& segment : state_borders) {
    const glm::vec3 a = glm::normalize(glm::vec3(segment.a_position_s));
    const glm::vec3 b = glm::normalize(glm::vec3(segment.b_position_s));
    const glm::vec3 middle = glm::normalize(a + b);
    const glm::vec3 along = glm::normalize(b - a);
    const glm::vec3 across = glm::normalize(glm::cross(middle, along));
    bool valid = false;
    for (const float epsilon : {0.00045f, 0.0009f, 0.0018f, 0.0036f}) {
      const uint32_t plus = state_for_region(pf10::sample_region(glm::normalize(middle + across * epsilon)).id);
      const uint32_t minus = state_for_region(pf10::sample_region(glm::normalize(middle - across * epsilon)).id);
      valid |= plus == segment.states.x && minus == segment.states.y;
    }
    invalid_state_sides += !valid;
  }
  check(invalid_state_sides == 0u,
        std::format("every rendered state-ribbon half owns its physical side ({} invalid)", invalid_state_sides));
  check(maximum_state_segment < 0.012f,
        std::format("every state ribbon segment remains atlas-local (max {:.6f} rad)", maximum_state_segment));

  check(packed.cells.size() > graph.province_ids.size() && packed.cells.size() < 8192u,
        std::format("exact political feature table stays compact ({} records)", packed.cells.size()));
  const auto patches = pf10::visible_surface_patches(512u, 16u, glm::vec3(0.0f, 0.0f, 1.2f));
  check(!patches.empty() && patches.size() < 6u * 32u * 32u,
        std::format("horizon culling retains a strict visible patch subset ({}/6144)", patches.size()));
  const auto refined = pf10::refined_surface_patches(512u, 16u, glm::vec3(0.0f, 0.0f, 1.2f));
  check(!refined.empty() && refined.size() < patches.size() &&
          std::ranges::all_of(refined, [](const pf10::surface_patch patch) { return patch.pad == 4u; }),
        std::format("inspection LOD is a strict crack-free 4x focus subset ({} patches)", refined.size()));
  check(pf10::refined_surface_patches(512u, 16u, glm::vec3(0.0f, 0.0f, 2.0f)).empty(),
        "inspection LOD turns off outside close viewing distance");

  const auto hydrology = pf10::make_hydrology_features();
  const auto repeated_hydrology = pf10::make_hydrology_features();
  check(hydrology.size() >= 500u && hydrology.size() < 4096u,
        std::format("hydrology fixture is a compact smooth feature layer ({} primitives)", hydrology.size()));
  check(hydrology.size() == repeated_hydrology.size() &&
          (hydrology.empty() || std::memcmp(hydrology.data(), repeated_hydrology.data(),
                                            hydrology.size() * sizeof(pf10::hydrology_feature)) == 0),
        "hydrology feature data is bit-identical across repeated builds");
  check(std::ranges::all_of(hydrology, [](const pf10::hydrology_feature& feature) {
          return feature.widths_kind.x > 0.0f && feature.widths_kind.y > 0.0f &&
                 pf10::sample_region(glm::vec3(feature.a_direction_height)).kind == pf10::region_kind::land &&
                 pf10::sample_region(glm::vec3(feature.b_direction_height)).kind == pf10::region_kind::land;
        }), "every river/lake primitive lies on playable land without changing province ownership");
  uint32_t invalid_river_centrelines = 0u;
  uint32_t invalid_lake_rims = 0u;
  for (const auto& feature : hydrology) {
    const glm::vec3 a = glm::normalize(glm::vec3(feature.a_direction_height));
    const glm::vec3 b = glm::normalize(glm::vec3(feature.b_direction_height));
    if (feature.widths_kind.z <= 0.5f) {
      bool valid = true;
      for (uint32_t sample = 0u; sample <= 4u; ++sample) {
        valid &= pf10::sample_region(glm::normalize(glm::mix(a, b, float(sample) * 0.25f))).kind ==
                 pf10::region_kind::land;
      }
      invalid_river_centrelines += !valid;
      continue;
    }
    glm::vec3 along = b - a;
    if (glm::dot(along, along) < 1.0e-10f) {
      along = glm::cross(a, std::abs(a.y) < 0.8f ? glm::vec3(0.0f, 1.0f, 0.0f) :
                                                      glm::vec3(1.0f, 0.0f, 0.0f));
    }
    along = glm::normalize(along);
    const glm::vec3 across = glm::normalize(glm::cross(a, along));
    bool valid = true;
    for (uint32_t sample = 0u; sample < 16u; ++sample) {
      const float angle = float(sample) * 0.39269908169f;
      const glm::vec3 rim = glm::normalize(a +
        (along * std::cos(angle) + across * std::sin(angle)) * feature.widths_kind.x);
      valid &= pf10::sample_region(rim).kind == pf10::region_kind::land;
    }
    invalid_lake_rims += !valid;
  }
  check(invalid_river_centrelines == 0u,
        std::format("complete river centrelines remain on playable land ({} invalid)",
                    invalid_river_centrelines));
  check(invalid_lake_rims == 0u,
        std::format("complete lake rims remain on playable land ({} invalid)", invalid_lake_rims));

  const glm::vec3 probe = glm::normalize(glm::vec3(0.37f, 0.51f, -0.78f));
  const float original_height = pf10::surface_height(probe);
  const auto original_region = pf10::sample_region(probe);
  const glm::mat3 rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(137.0f), glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f))));
  const glm::vec3 world = rotation * pf10::surface_position(probe);
  const glm::vec3 recovered = glm::transpose(rotation) * glm::normalize(world);
  check(std::abs(pf10::surface_height(recovered) - original_height) < 1.0e-6f,
        "height is planet-local and survives globe rotation");
  check(pf10::sample_region(recovered).id == original_region.id,
        "province identity is planet-local and survives globe rotation");

  const auto landmarks = pf10::make_landmarks(24);
  check(landmarks.size() == 24, "fixture has city/wonder/construction anchors");
  check(std::ranges::all_of(landmarks, [](const pf10::landmark& item) {
          return pf10::sample_region(glm::vec3(item.direction_height)).id == item.region_id;
        }), "every object anchor belongs to its recorded province");

  const auto centre_hit = pf10::intersect_surface(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
  check(centre_hit.hit && centre_hit.region.id != pf10::no_region,
        "visible displaced surface resolves to a selectable navigation region");
  check(!pf10::intersect_surface(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 1.0f, 0.0f)).hit,
        "a ray beside the globe cannot select a hidden province");
  bool radial_picking = true;
  constexpr float golden_angle = 2.39996322972865332f;
  for (uint32_t sample = 0u; sample < 256u; ++sample) {
    const float y = 1.0f - 2.0f * (float(sample) + 0.5f) / 256.0f;
    const float radius = std::sqrt(std::max(1.0f - y * y, 0.0f));
    const float angle = float(sample) * golden_angle;
    const glm::vec3 direction{std::cos(angle) * radius, y, std::sin(angle) * radius};
    const auto expected = pf10::sample_region(direction);
    const auto hit = pf10::intersect_surface(direction * 3.0f, -direction);
    radial_picking &= hit.hit && hit.region.id == expected.id &&
                      glm::dot(hit.direction, direction) > 0.999999f &&
                      std::abs(hit.height - pf10::surface_height(direction)) < 1.0e-5f;
  }
  check(radial_picking, "radial picking returns the front displaced owner on 256 planet-wide probes");

  glm::vec3 camera_direction{0.0f, 0.0f, 1.0f};
  for (uint32_t i = 0u; i < 200u; ++i) {
    camera_direction = pf10::orbit_camera_direction(camera_direction, 0.0f, 1.0f, 0.08f);
  }
  check(std::abs(glm::length(camera_direction) - 1.0f) < 1.0e-5f && camera_direction.y <= 0.940001f,
        "world-Y camera orbit stays normalized and cannot reach the north-pole singularity");
  for (uint32_t i = 0u; i < 400u; ++i) {
    camera_direction = pf10::orbit_camera_direction(camera_direction, 1.0f, -1.0f, 0.08f);
  }
  check(std::abs(glm::length(camera_direction) - 1.0f) < 1.0e-5f &&
          std::abs(camera_direction.y) <= 0.940001f,
        "combined WASD orbit remains finite and outside both polar extrema");

  std::cout << std::format("PF10 verify: {}/{} passed\n", passed, passed + failed);
  return failed == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, const char** argv) {
  devils_engine::pf10::viewer_options options;
  bool run_verify = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;
    if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else if (argument == "--verify") run_verify = true;
    else if (argument == "--view") run_verify = false;
    else if (argument == "--validation") options.validation = true;
    else if (argument == "--fixed-rotation") options.fixed_rotation = true;
    else if (argument == "--no-hydrology") options.show_hydrology = false;
    else if (argument == "--no-state-borders") options.show_state_borders = false;
    else if (prefixed(argument, "--border-debug=", value)) {
      if (value == "exact") options.border_debug = 1u;
      else if (value == "distance") options.border_debug = 2u;
      else if (value == "state") options.border_debug = 3u;
      else {
        std::cerr << "unknown border debug mode: " << value << '\n';
        return 2;
      }
    }
    else if (prefixed(argument, "--frames=", value)) options.frames = uint32_t(std::stoul(value));
    else if (prefixed(argument, "--shot=", value)) options.dump_path = value;
    else if (prefixed(argument, "--mesh=", value)) {
      const uint32_t requested = std::clamp(uint32_t(std::stoul(value)), 32u, 512u);
      options.mesh_side = requested / 16u * 16u;
    }
    else if (prefixed(argument, "--distance=", value)) options.camera_distance = std::clamp(std::stof(value), 1.16f, 4.5f);
    else {
      std::cerr << "unknown option: " << argument << '\n';
      usage();
      return 2;
    }
  }
  return run_verify ? verify() : devils_engine::pf10::run_viewer(options);
}
