#include "assets_system.h"

#include <vector>

#include <devils_engine/utils/core.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_loader.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <shaderc/shaderc.h>

#include "messages.h"
#include "message_dispatcher.h"
#include "mesh_resource.h"
#include "texture_resource.h"
#include "tile_map.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct assets_simulation_init {
  std::unique_ptr<demiurg::resource_system> resources;
  std::unique_ptr<demiurg::module_system> modules;
  demiurg::resource_loader loader;

  message_dispatcher<command_load_resource> load_commands;
  message_dispatcher<command_gpu_done> gpu_done_commands;
  message_dispatcher<command_load_chunk> chunk_commands;
  message_dispatcher<command_prepare_shaders> prepare_shader_commands;
  std::vector<command_load_resource> load_cache;
  std::vector<command_gpu_done> gpu_done_cache;
  std::vector<command_load_chunk> chunk_cache;
  std::vector<command_prepare_shaders> prepare_shader_cache;

  std::vector<demiurg::resource_loader::external_job> gpu_jobs;

  graphics_actor* gactor = nullptr;
};

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
assets_simulation::~assets_simulation() noexcept = default;

void assets_simulation::init() {
  container.reset(new assets_simulation_init);
  actor.add_receiver<command_load_resource>(&container->load_commands);
  actor.add_receiver<command_gpu_done>(&container->gpu_done_commands);
  actor.add_receiver<command_load_chunk>(&container->chunk_commands);
  actor.add_receiver<command_prepare_shaders>(&container->prepare_shader_commands);

  container->resources = std::make_unique<demiurg::resource_system>();
  container->resources->register_type<mesh_resource>("mesh", "mesh");
  container->resources->register_type<texture_resource>("textures", "png");

  // Реестр строим один раз здесь (init вызывается на главном потоке до старта потока ассетов).
  const auto modules_root = utils::project_folder() + "resources/modules/";
  container->modules = std::make_unique<demiurg::module_system>(modules_root);
  container->modules->load_default_modules();
  // resource_system::parse_resources сам делает open/parse/close модулей + финализацию
  // (дедуп replacement/supplementary + сортировка) — после него реестр готов к get/find.
  container->resources->parse_resources(container->modules.get());

  utils::info("assets: registry built from '{}', {} resources", modules_root, container->resources->resources_count());
}

bool assets_simulation::stop_predicate() const { return false; }

void assets_simulation::update(const size_t) {
  // ack'и от рендера: GPU-переход завершён
  dispatcher_consume(container->gpu_done_commands, container->gpu_done_cache, [this] (const auto& cmd) {
    container->loader.external_done(cmd.res);
  });

  // запросы от main: довести ресурс до target
  dispatcher_consume(container->load_commands, container->load_cache, [this] (const auto& cmd) {
    container->loader.request(cmd.res, cmd.target); // target — уровень FSM (int), клампится в request
  });

  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main actor.
  dispatcher_consume(container->chunk_commands, container->chunk_cache, [] (const auto& cmd) {
    if (cmd.reply_to == nullptr || cmd.size == 0) return;

    const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.texture_count);
    command_chunk_loaded out;
    out.x = chunk.coord.x;
    out.y = chunk.coord.y;
    out.size = chunk.size;
    out.textures.reserve(chunk.tiles.size());
    for (const auto& t : chunk.tiles) out.textures.push_back(t.texture);
    cmd.reply_to->send(std::move(out));
  });

  dispatcher_consume(container->prepare_shader_commands, container->prepare_shader_cache, [] (const auto& cmd) {
    command_shaders_prepared out;
    if (cmd.registry == nullptr) {
      if (cmd.reply_to != nullptr) cmd.reply_to->send(out);
      return;
    }

    std::vector<painter::glsl_source_file*> shaders;
    cmd.registry->template find<painter::glsl_source_file>(cmd.prefix, shaders);

    const auto infer_kind = [] (const std::string_view id) -> uint32_t {
      if (id.ends_with(".vert")) return shaderc_vertex_shader;
      if (id.ends_with(".frag")) return shaderc_fragment_shader;
      if (id.ends_with(".comp")) return shaderc_compute_shader;
      if (id.ends_with(".geom")) return shaderc_geometry_shader;
      if (id.ends_with(".tesc")) return shaderc_tess_control_shader;
      if (id.ends_with(".tese")) return shaderc_tess_evaluation_shader;
      return UINT32_MAX;
    };

    for (auto* shader : shaders) {
      if (shader == nullptr) continue;
      const uint32_t kind = infer_kind(shader->id);
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

    utils::info("assets: prepared shaders prefix '{}' compiled={} failed={}", cmd.prefix, out.compiled, out.failed);
    if (cmd.reply_to != nullptr) cmd.reply_to->send(out);
  });

  // reconcile: cold↔warm делаем сами, warm↔hot уходит в gpu_jobs
  container->gpu_jobs.clear();
  container->loader.update(container->gpu_jobs);

  for (const auto& job : container->gpu_jobs) {
    if (container->gactor != nullptr) {
      command_gpu_transition t{job.res, job.load};
      container->gactor->send(t);
    } else {
      // без рендера ресурс остаётся в warm — для headless-проверки cold→warm этого достаточно
      utils::info("assets: GPU transition needed (load={}) but no render actor; resource stays warm", job.load);
    }
  }
}

assets_actor* assets_simulation::get_actor() { return &actor; }

demiurg::resource_system* assets_simulation::resources() {
  return container ? container->resources.get() : nullptr;
}

void assets_simulation::set_render_actor(graphics_actor* gactor) {
  if (container) container->gactor = gactor;
}

}
}
