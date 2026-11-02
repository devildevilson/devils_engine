#include "local_structure.h"
#include "cave.h"
#include "world_chain.h"
#include "devils_engine/utils/core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "devils_engine/utils/prng.h"

namespace gn09 {
namespace {

double dot(const point a, const point b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
point subtract(const point a, const point b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

} // namespace

int8_t preferred_dock_axis(const uint64_t seed, const node& at) {
  const uint64_t pose = devils_engine::utils::mix(seed, at.key, 0x444f434bull) % 10u;
  const site_recipe recipe = recipe_of(seed, at);
  if (recipe == site_recipe::mining_base) return pose < 6u ? 0 : 1;
  if (recipe == site_recipe::residential_base) return pose < 8u ? -1 : 0;
  const cave_biome biome = biome_at(seed, at.centre);
  const uint64_t side_limit = biome == cave_biome::karst ? 6u :
                              biome == cave_biome::rift ? 4u : 2u;
  return pose < side_limit ? 0 :
         pose < side_limit + (10u - side_limit) / 2u ? 1 : -1;
}

local_structure materialize_local_structure(const adventure& world, const uint32_t node_id) {
  const site local = materialize_site(world, node_id);
  const node& at = world.route.nodes.at(node_id);
  local_structure result;
  result.node_id = node_id;
  result.kind = at.kind;
  result.recipe = local.recipe;
  result.centre = at.centre;

  // Предметы — выход шага features генератора cave (generator/cave/02_features.lua).
  if (!is_base(at.kind) && local.has_cave) {
    const auto instance = cave_instance(world, node_id);
    const auto room = instance->field("features", "room"), kind = instance->field("features", "kind"),
               amount = instance->field("features", "amount"), centre = instance->field("features", "centre");
    const auto& kinds = cave_feature_kinds();
    for (size_t i = 0; i < room.count(); ++i) {
      const auto code = size_t(kind.get(i));
      if (code >= kinds.size()) devils_engine::utils::error{}("GN09: cave feature kind {} is outside the vocabulary", code);
      result.features.push_back({uint32_t(room.get(i)), {centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)},
                                 kinds[code], uint32_t(amount.get(i))});
    }
  }
  return result;
}

std::string verify_local_structure(const adventure& world, const site& local,
                                   const local_structure& structure) {
  if (structure.node_id != local.node_id || structure.kind != local.kind ||
      structure.recipe != local.recipe ||
      structure.centre.x != local.centre.x || structure.centre.y != local.centre.y ||
      structure.centre.z != local.centre.z) return "local structure disagrees with site";
  const bool built_recipe = local.recipe == site_recipe::mining_base ||
    local.recipe == site_recipe::residential_base ||
    local.recipe == site_recipe::military_base;
  if (is_base(local.kind) != built_recipe) return "local recipe disagrees with site kind";
  if (!is_base(local.kind)) {
    // Каждый тупик с залежью, гнездо с кладкой, каждая горловина-пробка с меткой.
    std::vector<uint32_t> per_room(local.rooms.size(), 0);
    for (const local_feature& feature : structure.features) {
      if (feature.room < per_room.size()) ++per_room[feature.room];
    }
    for (uint32_t i = 1; i < local.rooms.size(); ++i) {
      const bool pocket = local.rooms[i].purpose.starts_with("pocket:") || local.rooms[i].purpose == "lair";
      const bool plug = std::find(local.plugs.begin(), local.plugs.end(), i) != local.plugs.end();
      if ((pocket || plug) != (per_room[i] == 1) && !(i == local.nest && per_room[i] >= 1)) {
        return "natural site has an invalid feature plan";
      }
    }
    for (const local_feature& feature : structure.features) {
      if (feature.room == 0 || feature.room >= local.rooms.size() || feature.amount == 0 ||
          feature.kind.empty()) return "local feature lacks a chamber or count";
      const point delta = subtract(feature.centre, local.rooms[feature.room].centre);
      if (dot(delta, delta) > local.rooms[feature.room].radius * local.rooms[feature.room].radius ||
          density(world, feature.centre) >= 0.0) return "local feature lies outside its chamber";
    }
    return {};
  }
  return {};
}

bool write_structure_svg(const adventure& world, const uint32_t node_id, const std::string& path) {
  if (node_id >= world.route.nodes.size() || world.route.nodes[node_id].kind == theme::empty) return false;
  if (is_base(world.route.nodes[node_id].kind)) return write_station_svg(world, node_id, path);
  const local_structure structure = materialize_local_structure(world, node_id);
  std::ofstream output(path);
  if (!output) return false;
  {
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-90 -90 180 180\" "
              "width=\"960\" height=\"960\">\n"
              "<rect x=\"-90\" y=\"-90\" width=\"180\" height=\"180\" fill=\"#1b2024\"/>\n";
    const site local = materialize_site(world, node_id);
    for (const room& chamber : local.rooms) {
      output << "<circle cx=\"" << chamber.centre.x - structure.centre.x << "\" cy=\""
             << chamber.centre.z - structure.centre.z << "\" r=\"" << chamber.radius
             << "\" fill=\"#314f5e\" stroke=\"#8ab5c7\" stroke-width=\"0.1\"/>\n";
    }
    for (const local_feature& feature : structure.features) {
      output << "<circle cx=\"" << feature.centre.x - structure.centre.x << "\" cy=\""
             << feature.centre.z - structure.centre.z << "\" r=\"0.45\" fill=\"#e4c46b\"/>\n"
             << "<text x=\"" << feature.centre.x - structure.centre.x + 0.7 << "\" y=\""
             << feature.centre.z - structure.centre.z << "\" fill=\"white\" font-size=\"0.7\">"
             << feature.kind << ':' << feature.amount << "</text>\n";
    }
  }
  output << "</svg>\n";
  return bool(output);
}

} // namespace gn09
