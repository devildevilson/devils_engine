#include "world_chain.h"

#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "cave.h"
#include "local_structure.h"
#include "station.h"
#include "tunnel.h"
#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/originator/generator_resource.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/utils/core.h"

namespace gn09 {

namespace originator = devils_engine::originator;

namespace {

std::vector<std::string>& world_mods() {
  static std::vector<std::string> list;
  return list;
}
std::atomic_bool world_read{false};

// Цепочка приезжает через demiurg — тем же путём, каким приехала бы из мода. Хост знает одно имя,
// `generator/world`, и имена генераторов в ней.
struct world_generator {
  devils_engine::demiurg::module_system modules;
  devils_engine::demiurg::resource_system resources;
  originator::loaded_chain chain;
  std::vector<std::string> purposes;
  std::vector<std::string> station_purposes;
  std::vector<std::string> feature_kinds;
  std::vector<std::string> base_purposes;

  world_generator() : modules(std::string(GN09_RESOURCE_ROOT) + "/") {
    // У demiurg при одинаковом id побеждает модуль, стоящий в списке РАНЬШЕ. Поэтому моды идут перед
    // gn09, а последний названный в --mods — первым.
    std::vector<devils_engine::demiurg::module_system::list_entry> list;
    for (auto mod = world_mods().rbegin(); mod != world_mods().rend(); ++mod) list.push_back({*mod, "", ""});
    list.push_back({"gn09/", "", ""});
    modules.load_modules(list);
    if (modules.loaded_modules().size() != list.size()) {
      devils_engine::utils::error{}("GN09: {} of {} generator modules loaded (a mod folder is missing?)",
                                    modules.loaded_modules().size(), list.size());
    }
    originator::register_generator_resources(resources);
    resources.parse_resources(&modules);
    chain = originator::load_chain(resources, "generator/world");
    const auto& constants = chain.setup.constants;
    for (const char* name : {"sector_size", "region_size", "surface_guard", "sub_length", "sub_beam",
         "sub_axis_half", "sub_path_radius", "sub_turn_radius", "macro_scale", "base_cavern_radius",
         "cave_extent", "site_reach", "sub_radius", "station_reach", "tunnel_partner_reach",
         "tunnel_max_slope_deg", "station_cell", "hall_reach", "pillar_flare"}) {
      if (!constants.has(name) || !std::isfinite(constants.number(name)) || constants.number(name) <= 0.0) {
        devils_engine::utils::error{}("GN09: global constant '{}' must be a finite positive number", name);
      }
    }
    for (const char* name : {"world_y_min", "world_y_max"}) {
      if (!constants.has(name) || !std::isfinite(constants.number(name))) {
        devils_engine::utils::error{}("GN09: global constant '{}' must be a finite number", name);
      }
    }
    for (const char* name : {"sector_size", "region_size", "world_y_min", "world_y_max"}) {
      if (std::floor(constants.number(name)) != constants.number(name)) {
        devils_engine::utils::error{}("GN09: global constant '{}' must be an integer", name);
      }
    }
    if (constants.number("world_y_min") >= constants.number("world_y_max") ||
        constants.number("sub_length") < constants.number("sub_beam") ||
        constants.number("surface_guard") * constants.number("surface_guard") <= 3.0) {
      devils_engine::utils::error{}("GN09: inconsistent world bounds, submarine dimensions or surface guard");
    }
    const std::string_view words = chain.config("cave").description.values.string("purposes");
    for (size_t at = 0; at < words.size();) {
      while (at < words.size() && std::isspace(static_cast<unsigned char>(words[at]))) ++at;
      const size_t start = at;
      while (at < words.size() && !std::isspace(static_cast<unsigned char>(words[at]))) ++at;
      if (at > start) purposes.emplace_back(words.substr(start, at - start));
    }
    if (purposes.empty()) devils_engine::utils::error{}("GN09: generator/cave declares no purposes");
    const std::string_view station_words = chain.config("station").description.values.string("purposes");
    for (size_t at = 0; at < station_words.size();) {
      while (at < station_words.size() && std::isspace(static_cast<unsigned char>(station_words[at]))) ++at;
      const size_t start = at;
      while (at < station_words.size() && !std::isspace(static_cast<unsigned char>(station_words[at]))) ++at;
      if (at > start) station_purposes.emplace_back(station_words.substr(start, at - start));
    }
    if (station_purposes.empty()) devils_engine::utils::error{}("GN09: generator/station declares no purposes");
    const std::string_view feature_words = chain.config("cave").description.values.string("feature_kinds");
    for (size_t at = 0; at < feature_words.size();) {
      while (at < feature_words.size() && std::isspace(static_cast<unsigned char>(feature_words[at]))) ++at;
      const size_t start = at;
      while (at < feature_words.size() && !std::isspace(static_cast<unsigned char>(feature_words[at]))) ++at;
      if (at > start) feature_kinds.emplace_back(feature_words.substr(start, at - start));
    }
    if (feature_kinds.empty()) devils_engine::utils::error{}("GN09: generator/cave declares no feature kinds");
    const std::string_view base_words = chain.config("base_site").description.values.string("purposes");
    for (size_t at = 0; at < base_words.size();) {
      while (at < base_words.size() && std::isspace(static_cast<unsigned char>(base_words[at]))) ++at;
      const size_t start = at;
      while (at < base_words.size() && !std::isspace(static_cast<unsigned char>(base_words[at]))) ++at;
      if (at > start) base_purposes.emplace_back(base_words.substr(start, at - start));
    }
  }
};

const world_generator& generator() {
  static const world_generator instance;
  world_read = true;
  return instance;
}

} // namespace

originator::generator_chain& world_chain(const double clearance_radius) {
  static std::mutex lock;
  static std::map<uint64_t, std::unique_ptr<originator::generator_chain>> chains;
  std::lock_guard guard(lock);
  auto& slot = chains[std::bit_cast<uint64_t>(clearance_radius)];
  if (slot == nullptr) {
    const auto& loaded = generator().chain;
    originator::chain_setup setup = loaded.setup;
    setup.vocabulary = &world_shapes(clearance_radius);
    setup.values.set_number("clearance_radius", clearance_radius);
    setup.values.set_number("chunk_cells", double(chunk::cells));
    setup.values.set_number("cave_core", cave_core(clearance_radius));
    for (const auto& [name, size] : std::initializer_list<std::pair<const char*, size_t>>{
           {"one", 1}, {"three", 3}, {"sixteen", 16}, {"region_capacity", 64}, {"cell_capacity", 1024},
           {"hall_capacity", 8}, {"hall_pillar_capacity", 128}, {"hall_ledge_capacity", 64},
           {"hall_niche_capacity", 32}, {"hall_loop_capacity", 32}, {"hall_outpost_capacity", 16}, {"route_node_capacity", 32},
           {"route_edge_capacity", 32}, {"graph_node_capacity", 8192}, {"graph_edge_capacity", 8192},
           {"room_capacity", 256}, {"link_capacity", 384}, {"pillar_capacity", 64}, {"gallery_capacity", 8192}, {"turn_capacity", 8192}, {"lobe_capacity", 16384}, {"pocket_capacity", 8192},
           {"place_capacity", 128}, {"creature_capacity", 4096}, {"window_place_capacity", 1024}, {"feature_capacity", 64}, {"base_room_capacity", 8},
           {"station_room_capacity", 32}, {"station_door_capacity", 64}, {"station_triangle_capacity", 65536},
           {"station_cut_capacity", 128},
           {"station_voxel_capacity", 262144}, {"station_keep_capacity", 512}, {"station_mask_cells", 144 * 96},
           {"two", 2}, {"tunnel_candidate_capacity", 128},
           {"tunnel_piece_capacity", 16}, {"tunnel_triangle_capacity", 65536}, {"tunnel_sample_capacity", 4096}, {"creature_probe_capacity", 4096}, {"chunk_side", size_t(chunk::cells + 1)}, {"chunk_padded", size_t(chunk::cells + 3)},
           {"surface_capacity", size_t(chunk::cells * chunk::cells * chunk::cells * 36)},
           {"middle_capacity", size_t(chunk::cells * chunk::cells * chunk::cells)}, {"structure_vertex_capacity", 262144}, {"prop_capacity", 256}, {"chunk_anchor_capacity", 64}}) {
      setup.sizes.set(name, size);
    }
    slot = std::make_unique<originator::generator_chain>(loaded.description, std::move(setup));
    slot->set_invoker_source(originator::make_chain_invokers(*slot, add_world_tools));
  }
  return *slot;
}

int sector_size() { static const auto value = int(generator().chain.setup.constants.number("sector_size")); return value; }
int region_size() { static const auto value = int(generator().chain.setup.constants.number("region_size")); return value; }
int world_y_min() { static const auto value = int(generator().chain.setup.constants.number("world_y_min")); return value; }
int world_y_max() { static const auto value = int(generator().chain.setup.constants.number("world_y_max")); return value; }
double surface_guard() { static const auto value = double(generator().chain.setup.constants.number("surface_guard")); return value; }
double starter_sub_length() { static const auto value = double(generator().chain.setup.constants.number("sub_length")); return value; }
double starter_sub_beam() { static const auto value = double(generator().chain.setup.constants.number("sub_beam")); return value; }
double starter_sub_axis_half() { static const auto value = double(generator().chain.setup.constants.number("sub_axis_half")); return value; }
double starter_sub_path_radius() { static const auto value = double(generator().chain.setup.constants.number("sub_path_radius")); return value; }
double starter_sub_turn_radius() { static const auto value = double(generator().chain.setup.constants.number("sub_turn_radius")); return value; }
double macro_scale() { static const auto value = double(generator().chain.setup.constants.number("macro_scale")); return value; }
double base_cavern_radius() { static const auto value = double(generator().chain.setup.constants.number("base_cavern_radius")); return value; }
double cave_extent() { static const auto value = double(generator().chain.setup.constants.number("cave_extent")); return value; }
double cave_reach() { static const auto value = double(generator().chain.setup.constants.number("site_reach")); return value; }
double cave_hull_radius() { static const auto value = double(generator().chain.setup.constants.number("sub_radius")); return value; }
double station_reach() { static const auto value = double(generator().chain.setup.constants.number("station_reach")); return value; }
double tunnel_partner_reach() { static const auto value = double(generator().chain.setup.constants.number("tunnel_partner_reach")); return value; }
double tunnel_max_slope_deg() { static const auto value = double(generator().chain.setup.constants.number("tunnel_max_slope_deg")); return value; }
double station_cell() { static const auto value = double(generator().chain.setup.constants.number("station_cell")); return value; }

const std::vector<std::string>& cave_purposes() { return generator().purposes; }
const std::vector<originator::chain_shape>& world_shape_kinds() { return generator().chain.description.shapes; }
const originator::chain_description& world_chain_description() { return generator().chain.description; }
const originator::parameters& world_constants() { return generator().chain.setup.constants; }
const std::vector<std::string>& station_purposes() { return generator().station_purposes; }
const std::vector<std::string>& cave_feature_kinds() { return generator().feature_kinds; }
const std::vector<std::string>& base_purposes() { return generator().base_purposes; }

originator::instance_key window_key(const graph::window area) {
  const uint64_t columns = uint64_t(area.max_x - area.min_x), rows = uint64_t(area.max_z - area.min_z);
  return {{uint64_t(int64_t(area.min_x)), uint64_t(int64_t(area.min_z)), columns | (rows << 32)}};
}

std::shared_ptr<const adventure> world_view(const originator::generator_result& window) {
  return window.view<adventure>([](const originator::generator_result& instance) {
    // Мир окна из буферов: граф генератора graph и залы регионов окна в порядке регионов (как у
    // halls_near). Радиус прохода — из обещаний окна: цепочка у каждого радиуса своя.
    const double clearance = instance.field("limits", "clearance").get(0);
    auto& chain = world_chain(clearance);
    auto world = std::make_shared<adventure>();
    world->seed = instance.seed();
    world->clearance_radius = clearance;
    world->window_key = instance.key().words;
    world->from_window = true;
    world->route = graph_from(*chain.get("graph", instance.seed(), instance.key()));
    const auto area = chain.get("area", instance.seed(), instance.key());
    const auto regions = area->field("area_regions", "key");
    for (size_t i = 0; i < regions.count(); ++i) {
      const originator::instance_key key{{originator::read_key_word(regions, i, 0), originator::read_key_word(regions, i, 1), 0}};
      const std::vector<hall> halls = halls_from(*chain.get("halls", instance.seed(), key));
      world->halls.insert(world->halls.end(), halls.begin(), halls.end());
    }
    return std::shared_ptr<const adventure>(std::move(world));
  });
}

graph graph_from(const originator::generator_result& instance) {
  graph out;
  const auto meta = [&](const std::string_view name) { return instance.field("graph_meta", name).get(0); };
  out.area = {int(meta("min_x")), int(meta("min_z")), int(meta("max_x")), int(meta("max_z"))};
  out.entry = uint32_t(meta("entry"));
  out.destination = uint32_t(meta("destination"));
  const auto key = instance.field("nodes", "key"), centre = instance.field("nodes", "centre"),
             kind = instance.field("nodes", "kind"), rank = instance.field("nodes", "rank");
  const auto count = size_t(meta("nodes"));
  out.nodes.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.nodes.push_back({uint32_t(i), originator::read_key_word(key, i),
                         {centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)}, theme(uint8_t(kind.get(i))),
                         uint8_t(rank.get(i))});
  }
  const auto a = instance.field("edges", "a"), b = instance.field("edges", "b");
  const auto edges = size_t(meta("edges"));
  out.edges.reserve(edges);
  for (size_t i = 0; i < edges; ++i) out.edges.push_back({uint32_t(a.get(i)), uint32_t(b.get(i))});
  return out;
}

void set_world_mods(const std::string_view comma_separated_folders) {
  if (world_read) devils_engine::utils::error{}("GN09: mods are set after the generator chain was read");
  for (size_t at = 0; at <= comma_separated_folders.size();) {
    const size_t end = std::min(comma_separated_folders.find(',', at), comma_separated_folders.size());
    std::string item(comma_separated_folders.substr(at, end - at));
    // Модуль адресуется от корня ресурсов, как папка mods/ у игры: demiurg не принимает модуль вне
    // корня (id модуля обязан быть путём от него).
    if (!item.empty()) world_mods().push_back(item.ends_with('/') ? item : item + "/");
    at = end + 1;
  }
}

} // namespace gn09
