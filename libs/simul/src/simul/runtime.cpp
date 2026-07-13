#include "devils_engine/simul/instance_layout.h"
#include "devils_engine/simul/interpolation.h"
#include "devils_engine/simul/lifecycle.h"
#include "devils_engine/simul/main_runtime.h"
#include "devils_engine/simul/render_runtime.h"
#include "devils_engine/simul/resource_access_scope.h"

namespace devils_engine {
namespace simul {
size_t frame_time_from_fps(const uint32_t fps) noexcept {
  const auto valid_fps = std::max(fps, 1u);
  return utils::round(double(utils::global_time_resolution) / double(valid_fps));
}

size_t thread_start_gap(const size_t frame_time, const uint32_t divisor) noexcept {
  const auto valid_divisor = std::max(divisor, 1u);
  return utils::round(double(frame_time) / double(valid_divisor));
}

std::string project_path(std::string path) {
  if (path.empty()) {
    return utils::project_folder();
  }
  if (path.front() == '/') {
    return path;
  }
  return utils::project_folder() + path;
}

std::string source_line(const uint32_t line) {
  return line == UINT32_MAX ? std::string{"unknown"} : std::to_string(line);
}

void preload_render_config_sources(demiurg::resource_system& resources) {
  std::vector<painter::render_config_source*> rc;
  resources.find<painter::render_config_source>("render_config", rc);
  for (auto* r : rc) {
    r->load(utils::safe_handle_t{});
  }
  DE_LOG(catalogue::log_domain::resource, flow,
         "engine registry: preloaded {} render-config sources", rc.size());
}

void standard_render_set_shader_sources_loaded(
  const demiurg::resource_system* reg,
  const bool load) {
  if (reg == nullptr) {
    return;
  }
  std::vector<painter::glsl_source_file*> glsl;
  reg->find<painter::glsl_source_file>("shaders", glsl);
  std::vector<painter::shader_source_file*> spv;
  reg->find<painter::shader_source_file>("shaders/spv", spv);
  const auto apply = [load](demiurg::resource_interface* r) {
    if (load) {
      r->load(utils::safe_handle_t{});
    } else {
      r->force_unload(utils::safe_handle_t{});
    }
  };
  for (auto* r : glsl) {
    apply(r);
  }
  for (auto* r : spv) {
    apply(r);
  }
  DE_LOG(catalogue::log_domain::render, flow,
         "render: shader sources {} ({} glsl + {} spv)",
         load ? "loaded" : "unloaded", glsl.size(), spv.size());
}

std::string_view to_string(const runtime_stage stage) noexcept {
  switch (stage) {
    case runtime_stage::bootstrap_ready: return "bootstrap_ready";
    case runtime_stage::systems_created: return "systems_created";
    case runtime_stage::systems_initialized: return "systems_initialized";
    case runtime_stage::workers_started: return "workers_started";
    case runtime_stage::main_loop: return "main_loop";
    case runtime_stage::workers_stopped: return "workers_stopped";
  }
  return "workers_stopped";
}

std::string_view to_string(const app_state state) noexcept {
  switch (state) {
    case app_state::boot: return "boot";
    case app_state::loading: return "loading";
    case app_state::game: return "game";
  }
  return "game";
}

float interpolation_alpha(const size_t elapsed, const size_t frame_time) noexcept {
  if (frame_time == 0) {
    return 1.0f;
  }
  const auto alpha = float(elapsed) / float(frame_time);
  return alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

bool resource_is_visible(
  const std::shared_ptr<const resource_access_scope>& scope,
  const demiurg::resource_handle handle) noexcept {
  return scope == nullptr || scope->contains(handle);
}

namespace instance_layout {
std::tuple<std::string_view, std::size_t>
parse_layout_atoms(const std::string_view& str, const std::span<atom>& out) {
  if (str.empty() || std::isdigit(static_cast<unsigned char>(str[0]))) {
    return {std::string_view{}, 0};
  }

  auto i = std::size_t{0};
  auto n = std::size_t{0};
  while (i < str.size() && n < out.size()) {
    const auto start = i;
    for (; i < str.size() && !std::isdigit(static_cast<unsigned char>(str[i])); ++i) {
    }
    for (; i < str.size() && std::isdigit(static_cast<unsigned char>(str[i])); ++i) {
    }

    const auto part = str.substr(start, i - start);
    const auto fmt = painter::format::from_string(part);
    if (fmt >= painter::format::count) {
      return {str.substr(start), n};
    }
    out[n++] = atom{
      painter::format::element_type(fmt),
      painter::format::el_count(fmt),
      painter::format::size(fmt)};
  }

  return {str.substr(i), n};
}

namespace match_error {
std::string_view to_string(const values v) noexcept {
  switch (v) {
    case ok: return "ok";
    case empty_layout: return "empty_layout";
    case parse_error: return "parse_error";
    case attribute_count_mismatch: return "attribute_count_mismatch";
    case attribute_mismatch: return "attribute_mismatch";
    case stride_mismatch: return "stride_mismatch";
    default: return "unknown";
  }
}
} // namespace match_error
} // namespace instance_layout
} // namespace simul
} // namespace devils_engine
