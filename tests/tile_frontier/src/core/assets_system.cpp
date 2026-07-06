#include <devils_engine/catalogue/logging.h>
#include "assets_system.h"

#include <vector>

#include <devils_engine/utils/core.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_loader.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <shaderc/shaderc.h>

#include "messages.h"
#include "broker.h"
#include <devils_engine/painter/mesh_resource.h>
#include <devils_engine/painter/texture_resource.h>
#include <devils_engine/sound/sound_resource.h>
#include <devils_engine/visage/font_resource.h>
#include "tile_map.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct assets_simulation_init {
  std::unique_ptr<demiurg::resource_system> resources;
  std::unique_ptr<demiurg::module_system> modules;
  demiurg::resource_loader loader;

  broker* br = nullptr; // все каналы — в общем broker (main владеет)
  std::vector<demiurg::resource_loader::external_job> gpu_jobs;
};

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
assets_simulation::~assets_simulation() noexcept = default;

void assets_simulation::init() {
  container.reset(new assets_simulation_init);

  container->resources = std::make_unique<demiurg::resource_system>();
  container->resources->register_type<painter::mesh_resource>("mesh", "mesh");
  // Регистрируем png-текстуру, но loading_type_id = БАЗА gpu_texture_resource: рендер/texture_set
  // работают через базу (нужен лишь gpu_index), не зная про конкретный png-декодер.
  container->resources->register_type<painter::gpu_texture_resource, painter::texture_resource>("textures", "png");
  // Шрифты — тоже контент (resources/modules/core/fonts/, моддятся как текстуры). Многошаговый
  // ресурс ttf→MSDF→GPU; loading_type_id = база gpu_texture_resource (рендер льёт атлас как
  // текстуру), точный тип достаётся через handle.get<visage::font_resource>() (lua/push_font).
  container->resources->register_type<painter::gpu_texture_resource, visage::font_resource>("fonts", "ttf");
  // Звуки — игровой контент (resources/modules/core/sounds/); тип матчится на сегмент "sounds".
  container->resources->register_type<sound::sound_resource>("sounds", "mp3,flac,wav,ogg");

  // Реестр строим один раз здесь (init вызывается на главном потоке до старта потока ассетов).
  const auto modules_root = utils::project_folder() + "resources/modules/";
  container->modules = std::make_unique<demiurg::module_system>(modules_root);
  container->modules->load_default_modules();
  // resource_system::parse_resources сам делает open/parse/close модулей + финализацию
  // (дедуп replacement/supplementary + сортировка) — после него реестр готов к get/find.
  container->resources->parse_resources(container->modules.get());

  DE_LOG(catalogue::log_domain::assets, flow, "assets: registry built from '{}', {} resources", modules_root, container->resources->resources_count());
}

bool assets_simulation::stop_predicate() const { return false; }

void assets_simulation::update(const size_t) {
  if (container->br == nullptr) return; // broker ещё не задан
  broker& br = *container->br;

  // ack'и от рендера: GPU-переход завершён
  {
    command_gpu_done cmd{};
    while (br.gpu_done.try_pop(cmd)) {
      auto* res = cmd.res.get();
      if (res == nullptr) { utils::warn("assets: gpu_done with unresolved resource handle"); continue; }
      container->loader.external_done(res);
    }
  }

  // запросы от main: довести ресурс до target
  {
    command_load_resource cmd{};
    while (br.load_resource.try_pop(cmd)) {
      auto* res = cmd.res.get();
      if (res == nullptr) { utils::warn("assets: load_resource with unresolved resource handle"); continue; }
      container->loader.request(res, cmd.target); // target — уровень FSM (int), клампится в request
    }
  }

  // mock world streaming: CPU-чанк генерируется на assets thread и возвращается main через broker.
  {
    command_load_chunk cmd{};
    while (br.load_chunk.try_pop(cmd)) {
      if (cmd.size == 0) continue;
      const tile_chunk chunk = generate_mock_chunk(chunk_coord{cmd.x, cmd.y}, cmd.size, cmd.texture_count);
      command_chunk_loaded out;
      out.x = chunk.coord.x;
      out.y = chunk.coord.y;
      out.size = chunk.size;
      out.textures.reserve(chunk.tiles.size());
      for (const auto& t : chunk.tiles) out.textures.push_back(t.texture);
      br.chunk_loaded.try_push(std::move(out));
    }
  }

  // компиляция шейдеров (CPU-heavy) на потоке ассетов; результат — latest-wins в render.
  {
    command_prepare_shaders cmd{};
    while (br.prepare_shaders.try_pop(cmd)) {
      command_shaders_prepared out;
      if (cmd.registry == nullptr) { br.shaders_prepared.write_slot() = out; br.shaders_prepared.publish(); continue; }

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

      DE_LOG(catalogue::log_domain::assets, flow, "assets: prepared shaders prefix '{}' compiled={} failed={}", cmd.prefix, out.compiled, out.failed);
      br.shaders_prepared.write_slot() = out;
      br.shaders_prepared.publish();
    }
  }

  // reconcile: cold↔warm делаем сами, warm↔hot уходит в gpu_jobs → broker рендеру
  container->gpu_jobs.clear();
  container->loader.update(container->gpu_jobs);

  for (const auto& job : container->gpu_jobs) {
    br.gpu_transition.try_push(command_gpu_transition{resource_ref::from_system(container->resources.get(), job.res), job.load});
  }
}

assets_actor* assets_simulation::get_actor() { return &actor; }

demiurg::resource_system* assets_simulation::resources() {
  return container ? container->resources.get() : nullptr;
}

void assets_simulation::set_broker(broker* b) {
  if (container) container->br = b;
}

}
}
