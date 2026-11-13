#include "runtime.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/script_shape.h"
namespace gn09_next {
o::instance_key key(int64_t x, int64_t z) {
  return {{uint64_t(x), uint64_t(z), 0}};
}
o::instance_key key(int64_t x, int64_t y, int64_t z) {
  return {{uint64_t(x), uint64_t(y), uint64_t(z)}};
}
runtime::runtime(const std::string& module, bool natural, bool caves_only)
    : generator_(caves_only ? "cave_landscape"
              : natural     ? "natural_landscape"
                            : "landscape"),
      modules_(std::string(GN09_RESOURCE_ROOT) + "/") {
  modules_.load_modules({{module + "/", "", ""}});
  o::register_generator_resources(resources_);
  resources_.parse_resources(&modules_);
  loaded_ = o::load_chain(resources_, "generator/world");
  o::configure_script_field(vocabulary_, loaded_.description, &loaded_.setup.constants);
  for (const auto& shape : loaded_.description.shapes)
    o::add_script_kind(vocabulary_, shape);
  auto setup = loaded_.setup;
  setup.vocabulary = &vocabulary_;
  setup.values.set_number("chunk_cells", chunk_cells);
  // Ёмкости буферов и размер чанка — параметры хоста: правила мира их не задают.
  setup.values.set_number("biome_map_side", biome_map_side);
  const std::initializer_list<std::pair<const char*, size_t>> sizes{{"cave_node_capacity", 96},
      {"cave_edge_capacity", 192}, {"cave_chamber_capacity", 64}, {"cave_passage_capacity", 1024},
      {"cave_probe_capacity", 2048}, {"cave_portal_capacity", 256}, {"cave_detail_capacity", 8192},
      {"feature_capacity", 2048}, {"feature_probe_capacity", 131072}, {"feature_key_capacity", 256},
      {"small_cave_scope_capacity", 512}, {"biome_map_side", biome_map_side}, {"biome_source_capacity", 49},
      {"outpost_ray_capacity", 16}, {"outpost_dock_capacity", 96}, {"outpost_body_capacity", 64},
      {"outpost_probe_capacity", 32768}, {"terrain_capacity", 1024}, {"terrain_scope_capacity", 225},
      {"network_node_capacity", 256}, {"network_edge_capacity", 256}, {"network_route_capacity", 64},
      {"wall_capacity", 512}, {"station_detail_capacity", 4096}, {"station_opening_capacity", 1024}, {"one", 1},
      {"shape_capacity", 32}, {"chunk_padded", chunk_cells + 3}, {"surface_capacity", 524288},
      {"structure_vertex_capacity", 65536}, {"prop_capacity", 128}, {"room_capacity", 96},
      {"station_triangle_capacity", 12288}};
  for (const auto& [name, count] : sizes)
    setup.sizes.set(name, count);
  chain_ = std::make_unique<o::generator_chain>(loaded_.description, std::move(setup));
  auto source = o::make_chain_invokers(*chain_, [this](o::tool_registry& tools) {
    o::tool_registry all;
    all.add_standard_tools();
    all.add_field_tools();
    all.add_volume_tools();
    all.add_graph_tools();
    for (size_t i = 0; i < all.size(); ++i) {
      auto description = all.at(i);
      if (description.body) {
        description.body = [this, body = description.body](
                               const o::tool_call& call, const size_t begin, const size_t end) {
          const auto began = std::chrono::steady_clock::now();
          body(call, begin, end);
          std::string name(call.step_name);
          name += '/';
          name += call.tool_name;
          add_timing(name,
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began)
                      .count()));
        };
      }
      tools.add(std::move(description));
    }
  });
  chain_->set_invoker_source([this, source = std::move(source)](const o::chain_generator& generator,
                                 o::generation_context& context) -> o::step_invoker {
    return [this, inner = source(generator, context), generator_name = generator.name](const o::step_context& step) {
      const auto began = std::chrono::steady_clock::now();
      inner(step);
      add_timing(generator_name + "/" + std::string(step.name),
          uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count()));
    };
  });
}
void runtime::add_timing(std::string_view name, uint64_t ns) {
  std::lock_guard guard(timings_lock_);
  auto found = timings_.find(name);
  if (found == timings_.end()) found = timings_.emplace(std::string(name), timing{}).first;
  ++found->second.calls;
  found->second.ns += ns;
}
void runtime::reset_timings() {
  std::lock_guard guard(timings_lock_);
  timings_.clear();
}
void runtime::report_timings() const {
  std::vector<std::pair<std::string, timing>> rows;
  {
    std::lock_guard guard(timings_lock_);
    rows.assign(timings_.begin(), timings_.end());
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.ns > b.second.ns;
  });
  std::cout << "Step and tool wall time (total s, calls, ms per call):\n";
  for (const auto& [name, t] : rows)
    if (t.ns >= 10'000'000)
      std::cout << "  " << name << ": " << double(t.ns) / 1e9 << " s, " << t.calls << ", "
                << double(t.ns) / 1e6 / double(t.calls) << " ms\n";
}
std::array<double, 3> runtime::start(uint64_t seed, int64_t x, int64_t z) {
  auto node = chain_->get("network", seed, key(x, z))->field("network_node", "centre");
  return {node.get(0, 0), node.get(0, 1), node.get(0, 2)};
}
o::result_ptr runtime::chunk(uint64_t seed, const o::chunk_key& at) {
  return chain_->get(generator_, seed, key(at.x, at.y, at.z));
}
devils_engine::gn03::generator_factory runtime::factory(uint64_t seed) {
  return [this, seed] {
    return [this, seed](const o::chunk_key& at, devils_engine::gn03::chunk_mesh& mesh) {
      const auto began = std::chrono::steady_clock::now();
      const auto result = chunk(seed, at);
      const auto biome = size_t(result->field("chunk_meta", "biome").get(0));
      if (biome < biome_chunks_.size()) ++biome_chunks_[biome];
      mesh.key = at;
      mesh.vertices.clear();
      mesh.props.clear();
      for (const char* buffer : {"surface", "structure_surface"}) {
        const auto p = result->field(buffer, "position"), n = result->field(buffer, "normal"),
                   shade = result->field(buffer, "shade");
        const size_t count = p.count(), first = mesh.vertices.size();
        mesh.vertices.resize(first + count);
        for (size_t i = 0; i < count; ++i) {
          auto& v = mesh.vertices[first + i];
          for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t chunk_axis = axis == 0 ? at.x : axis == 1 ? at.y : at.z;
            const double origin = double(chunk_axis) * chunk_cells;
            v.position[axis] = devils_engine::gn03::encode_local_position(p.get(i, axis) - origin, chunk_cells);
            v.normal[axis] = int8_t(std::lround(std::clamp(n.get(i, axis), -1.0, 1.0) * 127.0));
          }
          v.chunk = 0;
          v.padding = int8_t(shade.get(i));
        }
      }
      const auto props = result->field("props", "position"), kind = result->field("props", "kind");
      for (size_t i = 0; i < props.count(); ++i) {
        devils_engine::gn03::chunk_prop prop;
        for (uint32_t a = 0; a < 3; ++a) {
          const int64_t chunk_axis = a == 0 ? at.x : a == 1 ? at.y : at.z;
          prop.position[a] = props.get(i, a) - double(chunk_axis) * chunk_cells;
        }
        prop.kind = uint32_t(kind.get(i));
        prop.origin = uint32_t(i);
        prop.normal[1] = 1;
        mesh.props.push_back(prop);
      }
      mesh.biomes_used = 1;
      mesh.field_milliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    };
  };
}
std::string_view runtime::shape_source(std::string_view name) const {
  for (const auto& shape : loaded_.description.shapes)
    if (shape.name == name) return shape.text;
  throw std::runtime_error("GN09: world.tavl has no shape kind '" + std::string(name) + "'");
}
void runtime::report() const {
  std::cout << "Biome chunk builds (gallery/clutter/hall/ocean):";
  for (const auto& count : biome_chunks_)
    std::cout << ' ' << count.load();
  std::cout << '\n';
  for (const auto& [name, s] : chain_->all_stats())
    if (s.builds)
      std::cout << "  " << name << ": " << s.builds << " builds, self " << double(s.self_ns) / 1e6 / s.builds
                << " ms\n";
}
} // namespace gn09_next
