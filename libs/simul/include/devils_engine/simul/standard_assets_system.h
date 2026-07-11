#ifndef DEVILS_ENGINE_SIMUL_STANDARD_ASSETS_SYSTEM_H
#define DEVILS_ENGINE_SIMUL_STANDARD_ASSETS_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <shaderc/shaderc.h>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_loader.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <devils_engine/painter/mesh_resource.h>
#include <devils_engine/painter/texture_resource.h>
#include <devils_engine/simul/lua_script_resource.h>
#include <devils_engine/simul/startup_resources.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/visage/font_resource.h>

#include "messages.h"
#include "systems.h"

namespace devils_engine {
namespace simul {

template <typename Broker>
class standard_assets_system : public assets_system<Broker> {
public:
  using base_type = assets_system<Broker>;

  explicit standard_assets_system(const size_t frame_time) noexcept : base_type(frame_time) {}
  ~standard_assets_system() noexcept override = default;

  void init() override {
    container.reset(new state);

    container->resources = std::make_unique<demiurg::resource_system>();
    register_standard_resource_types(*container->resources);
    register_project_resource_types(*container->resources);

    const auto root = modules_root();
    container->modules = std::make_unique<demiurg::module_system>(root);
    load_modules(*container->modules);
    container->resources->parse_resources(container->modules.get());

    DE_LOG(catalogue::log_domain::assets, flow, "assets: registry built from '{}', {} resources", root, container->resources->resources_count());
  }

  bool stop_predicate() const override { return false; }

  void update(const size_t time) override {
    if (container == nullptr || this->broker_ == nullptr) return;
    auto& br = *this->broker_;

    drain_gpu_done(br);
    drain_load_resource(br);
    update_project(time, br);
    drain_prepare_shaders(br);
    reconcile_gpu_jobs(br);
  }

  demiurg::resource_system* resources() {
    return container ? container->resources.get() : nullptr;
  }

  const demiurg::resource_system* resources() const {
    return container ? container->resources.get() : nullptr;
  }

protected:
  virtual void register_project_resource_types(demiurg::resource_system&) {}

  virtual std::string modules_root() const {
    return utils::project_folder() + "resources/modules/";
  }

  virtual void load_modules(demiurg::module_system& modules) {
    modules.load_default_modules();
  }

  virtual void update_project(const size_t, Broker&) {}

  static void register_standard_resource_types(demiurg::resource_system& resources) {
    resources.register_type<painter::mesh_resource>("mesh", "mesh");
    // Consumers usually need only gpu_index, so texture/font concrete loaders are registered under
    // the common gpu_texture_resource loading type.
    resources.register_type<painter::gpu_texture_resource, painter::texture_resource>("textures", "png");
    resources.register_type<painter::gpu_texture_resource, visage::font_resource>("fonts", "ttf");
    resources.register_type<sound::sound_resource>("sounds", "mp3,flac,wav,ogg,opus");
    resources.register_type<lua_script_resource>("ui", "lua");
    resources.register_type<startup_entry_resource>("startup", "tavl");
    resources.register_type<runtime_state_resource>("states", "tavl");
  }

private:
  struct state {
    std::unique_ptr<demiurg::resource_system> resources;
    std::unique_ptr<demiurg::module_system> modules;
    demiurg::resource_loader loader;
    std::vector<demiurg::resource_loader::external_job> gpu_jobs;
  };

  void drain_gpu_done(Broker& br) {
    command_gpu_done cmd{};
    while (br.gpu_done.try_pop(cmd)) {
      auto* res = cmd.res.get();
      if (res == nullptr) {
        utils::warn("assets: gpu_done with unresolved resource handle");
        continue;
      }
      container->loader.external_done(res);
    }
  }

  void drain_load_resource(Broker& br) {
    command_load_resource cmd{};
    while (br.load_resource.try_pop(cmd)) {
      auto* res = cmd.res.get();
      if (res == nullptr) {
        utils::warn("assets: load_resource with unresolved resource handle");
        continue;
      }
      container->loader.request(res, cmd.target);
    }
  }

  void drain_prepare_shaders(Broker& br) {
    command_prepare_shaders cmd{};
    while (br.prepare_shaders.try_pop(cmd)) {
      command_shaders_prepared out;
      if (cmd.registry == nullptr) {
        br.shaders_prepared.write_slot() = out;
        br.shaders_prepared.publish();
        continue;
      }

      std::vector<painter::glsl_source_file*> shaders;
      cmd.registry->template find<painter::glsl_source_file>(cmd.prefix, shaders);

      for (auto* shader : shaders) {
        if (shader == nullptr) continue;
        const uint32_t kind = infer_shader_kind(shader->id);
        if (kind == UINT32_MAX) {
          utils::warn("assets: skip shader '{}' - cannot infer shader stage from id", shader->id);
          continue;
        }

        std::string err;
        if (shader->prepare_spirv(cmd.registry, kind, &err)) {
          out.compiled += 1;
        } else {
          out.failed += 1;
          utils::warn("assets: shader '{}' compilation failed: {}", shader->id, err);
        }
      }

      DE_LOG(catalogue::log_domain::assets, flow, "assets: prepared shaders prefix '{}' compiled={} failed={}", cmd.prefix, out.compiled, out.failed);
      br.shaders_prepared.write_slot() = out;
      br.shaders_prepared.publish();
    }
  }

  void reconcile_gpu_jobs(Broker& br) {
    container->gpu_jobs.clear();
    container->loader.update(container->gpu_jobs);

    for (const auto& job : container->gpu_jobs) {
      br.gpu_transition.try_push(command_gpu_transition{
        resource_ref::from_system(container->resources.get(), job.res),
        job.load
      });
    }
  }

  static uint32_t infer_shader_kind(const std::string_view id) {
    if (id.ends_with(".vert")) return shaderc_vertex_shader;
    if (id.ends_with(".frag")) return shaderc_fragment_shader;
    if (id.ends_with(".comp")) return shaderc_compute_shader;
    if (id.ends_with(".geom")) return shaderc_geometry_shader;
    if (id.ends_with(".tesc")) return shaderc_tess_control_shader;
    if (id.ends_with(".tese")) return shaderc_tess_evaluation_shader;
    return UINT32_MAX;
  }

  std::unique_ptr<state> container;
};

}
}

#endif
