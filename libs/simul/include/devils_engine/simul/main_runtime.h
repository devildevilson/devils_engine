#ifndef DEVILS_ENGINE_SIMUL_MAIN_RUNTIME_H
#define DEVILS_ENGINE_SIMUL_MAIN_RUNTIME_H

// Shared bootstrap helpers for project main loops and engine registries.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_system.h>
#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <devils_engine/painter/pipeline_cache_resource.h>
#include <devils_engine/painter/render_config_source.h>
#include <devils_engine/painter/shader_source_file.h>
#include <devils_engine/simul/boot_config.h>
#include <devils_engine/simul/render_config.h>
#include <devils_engine/simul/systems.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/fileio.h>
#include <devils_engine/utils/time-utils.hpp>
#include <tavl/deserialize.h>
#include <tavl/serialize.h>

namespace devils_engine {
namespace simul {

template <typename Settings>
struct standard_runtime_bootstrap {
  engine_boot_config engine;
  Settings settings;

  std::unique_ptr<demiurg::module_system> engine_modules;
  std::unique_ptr<demiurg::resource_system> engine_resources;
  std::unique_ptr<demiurg::module_system> cache_modules;

  std::unique_ptr<thread::atomic_pool> pool_container;
  thread::atomic_pool* pool = nullptr;
};

size_t frame_time_from_fps(uint32_t fps) noexcept;
size_t thread_start_gap(size_t frame_time, uint32_t divisor) noexcept;
std::string project_path(std::string path);

template <typename Bootstrap>
std::string settings_path(const Bootstrap& boot) {
  if (boot.engine.settings_file.empty()) {
    return utils::project_folder() + "settings.tavl";
  }
  if (boot.engine.settings_file.front() == '/') {
    return boot.engine.settings_file;
  }
  return utils::project_folder() + boot.engine.settings_file;
}

std::string source_line(uint32_t line);

template <typename Settings>
void sync_engine_boot_config(engine_boot_config& engine, const Settings& settings) {
  engine.render_enabled = settings.render.enabled;
  engine.sound_enabled = settings.simulation.sound_enabled;
  if constexpr (requires { settings.simulation.assets_enabled; }) {
    engine.assets_enabled = settings.simulation.assets_enabled;
  }
  engine.headless = settings.render.headless;
  engine.main_fps = settings.simulation.main_fps;
  engine.render_fps = settings.simulation.render_fps;
  engine.sound_fps = settings.simulation.sound_fps;
  engine.assets_fps = settings.simulation.assets_fps;
  engine.worker_threads_reserved = settings.simulation.worker_threads_reserved;
  engine.min_worker_threads = settings.simulation.min_worker_threads;
  engine.thread_start_gap_divisor = settings.simulation.thread_start_gap_divisor;
  engine.cache_root = settings.render.cache_folder;
}

template <typename Bootstrap>
void sync_engine_boot_config(Bootstrap& boot) {
  sync_engine_boot_config(boot.engine, boot.settings);
}

template <typename LoggingConfig>
void setup_logging(const LoggingConfig& log_cfg) {
  std::string file;
  if (!log_cfg.file.empty()) {
    file = utils::project_folder() + log_cfg.file;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
  }

  catalogue::init_logging(file, log_cfg.console);
  catalogue::register_engine_domains();

  const auto apply = [](const uint32_t id, const std::string& depth_str) {
    catalogue::log_depth d = catalogue::log_depth::off;
    if (catalogue::parse_log_depth(depth_str, d)) {
      catalogue::logs().set_level(id, d);
    } else {
      utils::warn("logging: unknown depth '{}' for domain '{}'", depth_str, catalogue::logs().name(id));
    }
  };

  namespace ld = catalogue::log_domain;
  apply(ld::main, log_cfg.main);
  apply(ld::assets, log_cfg.assets);
  apply(ld::sound, log_cfg.sound);
  apply(ld::render, log_cfg.render);
  apply(ld::ui, log_cfg.ui);
  apply(ld::gameplay, log_cfg.gameplay);
  apply(ld::resource, log_cfg.resource);
  apply(ld::demiurg, log_cfg.demiurg);
}

template <typename Bootstrap>
bool deserialize_settings_file(Bootstrap& boot, const std::string& path) {
  if (!file_io::exists(path)) {
    return false;
  }

  tavl::parser parser;
  const auto content = file_io::read(path);
  parser.add_default_operator();
  parser.flush(content);
  parser.finish();

  tavl::ct_context ctx;
  tavl::deserialize(parser, ctx, boot.settings);
  if (!ctx.diagnostics.empty()) {
    utils::warn("settings '{}': {} tavl diagnostics", path, ctx.diagnostics.size());
    for (const auto& d : ctx.diagnostics) {
      utils::warn("  tavl diagnostic '{}' at {}:{} field '{}'",
                  tavl::to_string(d.error.type), source_line(static_cast<uint32_t>(d.error.span.line)), d.error.span.column, d.field);
    }
  }

  sync_engine_boot_config(boot);
  return true;
}

template <typename Bootstrap>
bool save_settings(Bootstrap& boot) {
  const auto path = settings_path(boot);
  std::string tavl_data;
  if (!tavl::serialize(boot.settings, tavl_data)) {
    utils::warn("settings: could not serialize settings to tavl");
    return false;
  }

  const bool written = file_io::write(tavl_data, path);
  if (!written) {
    utils::warn("settings: could not write '{}'", path);
  } else {
    DE_LOG(catalogue::log_domain::main, flow, "Saved settings '{}'", path);
  }
  return written;
}

template <typename Bootstrap>
bool reload_settings(Bootstrap& boot) {
  const auto path = settings_path(boot);
  if (!deserialize_settings_file(boot, path)) {
    utils::warn("settings: '{}' not found, keeping current settings", path);
    return false;
  }
  DE_LOG(catalogue::log_domain::main, flow, "Reloaded settings '{}'", path);
  return true;
}

template <typename AppConfigResource, typename Bootstrap>
void register_standard_engine_resources(Bootstrap& boot) {
  boot.engine_resources->template register_type<AppConfigResource>("config", "tavl");
  boot.engine_resources->template register_type<painter::render_config_source>("render_config", "tavl");
  boot.engine_resources->template register_type<painter::glsl_source_file>("shaders", "glsl");
  boot.engine_resources->template register_type<painter::shader_source_file>("spv", "spv");
  boot.engine_resources->template register_type<painter::pipeline_cache_resource>("pipeline_cache", "bin");
}

void preload_render_config_sources(demiurg::resource_system& resources);

template <typename AppConfigResource, typename Bootstrap>
void init_standard_bootstrap(Bootstrap& boot) {
  boot.engine_resources = std::make_unique<demiurg::resource_system>();
  register_standard_engine_resources<AppConfigResource>(boot);

  boot.engine_modules = std::make_unique<demiurg::module_system>(utils::project_folder() + boot.engine.resource_root);
  boot.engine_modules->load_modules({demiurg::module_system::list_entry{boot.engine.engine_module, "", ""}});
  boot.engine_resources->parse_resources(boot.engine_modules.get());

  preload_render_config_sources(*boot.engine_resources);

  const auto config_path = utils::project_folder() + boot.engine.resource_root + boot.engine.engine_module + "/config/app.tavl";
  if (auto* cfg_res = boot.engine_resources->template get<AppConfigResource>(boot.engine.app_config_id)) {
    cfg_res->load(utils::safe_handle_t{});
    boot.settings = cfg_res->config();
  } else {
    utils::warn("engine registry: '{}' not found, using app_config defaults", boot.engine.app_config_id);
  }

  sync_engine_boot_config(boot);
  const auto user_settings_path = settings_path(boot);
  if (deserialize_settings_file(boot, user_settings_path)) {
    DE_LOG(catalogue::log_domain::main, flow, "Loaded user settings '{}'", user_settings_path);
  }

  setup_logging(boot.settings.logging);

  const uint32_t hw_threads = std::max(std::thread::hardware_concurrency(), 1u);
  uint32_t reserved_threads = boot.engine.worker_threads_reserved;
  if (!boot.engine.render_enabled && reserved_threads > 0) {
    reserved_threads -= 1;
  }
  if (!boot.engine.sound_enabled && reserved_threads > 0) {
    reserved_threads -= 1;
  }
  if (!boot.engine.assets_enabled && reserved_threads > 0) {
    reserved_threads -= 1;
  }
  const uint32_t min_worker_threads = std::max(boot.engine.min_worker_threads, 1u);
  const uint32_t thread_count = std::max(
    hw_threads > reserved_threads ? hw_threads - reserved_threads : min_worker_threads,
    min_worker_threads);

  const auto cpu_name = utils::get_cpu_name();
  utils::info("Using cpu '{}', cores: {}, worker threads: {}", cpu_name, hw_threads, thread_count);
  DE_LOG(catalogue::log_domain::main, flow,
         "Loaded app config '{}': window {}x{}, render config '{}', GPU preference '{}' / index {}",
         config_path,
         boot.settings.window.width,
         boot.settings.window.height,
         boot.settings.render.config_folder,
         boot.settings.render.preferred_gpu,
         boot.settings.render.preferred_gpu_index);

  boot.pool_container.reset(new thread::atomic_pool(thread_count));
  boot.pool = boot.pool_container.get();
}

template <typename Bootstrap>
void configure_standard_worker_pool(Bootstrap& boot, const size_t worker_count) {
  constexpr int64_t standard_worker_slots = 3;
  const int64_t configured = int64_t(boot.engine.worker_threads_reserved);
  const int64_t topology_delta = int64_t(worker_count) - standard_worker_slots;
  const uint32_t reserved_threads = uint32_t(std::max<int64_t>(configured + topology_delta, 1));
  const uint32_t hw_threads = std::max(std::thread::hardware_concurrency(), 1u);
  const uint32_t min_worker_threads = std::max(boot.engine.min_worker_threads, 1u);
  const uint32_t thread_count = std::max(
    hw_threads > reserved_threads ? hw_threads - reserved_threads : min_worker_threads,
    min_worker_threads);

  if (boot.pool_container != nullptr && boot.pool_container->size() == thread_count) {
    return;
  }

  DE_LOG(catalogue::log_domain::main, flow,
         "Runtime topology: {} worker systems, {} reserved threads, {} pool threads",
         worker_count,
         reserved_threads,
         thread_count);
  boot.pool_container = std::make_unique<thread::atomic_pool>(thread_count);
  boot.pool = boot.pool_container.get();
}

template <typename AppConfigResource>
struct standard_app_runtime_traits {
  template <typename Bootstrap>
  static void init_bootstrap(Bootstrap& boot) {
    init_standard_bootstrap<AppConfigResource>(boot);
  }

  template <typename Bootstrap>
  static bool save_settings(Bootstrap& boot) {
    return simul::save_settings(boot);
  }

  template <typename Bootstrap>
  static bool reload_settings(Bootstrap& boot) {
    return simul::reload_settings(boot);
  }

  template <typename Bootstrap, typename Workers>
  static void configure_topology(Bootstrap& boot, const Workers& workers) {
    configure_standard_worker_pool(boot, workers.size());
  }
};

template <typename Bootstrap>
void prepare_pipeline_cache(Bootstrap& boot, const std::string& pipeline_cache_id) {
  const std::string cache_module = boot.settings.render.cache_folder + "/painter";
  file_io::create_directory(project_path(boot.settings.render.cache_folder));
  file_io::create_directory(project_path(cache_module));
  file_io::create_directory(project_path(cache_module + "/pipeline_cache"));

  boot.cache_modules = std::make_unique<demiurg::module_system>(utils::project_folder());
  boot.cache_modules->load_modules({demiurg::module_system::list_entry{cache_module, "", ""}});
  boot.engine_resources->append_resources(boot.cache_modules.get());
  if (auto* pc = boot.engine_resources->template get<painter::pipeline_cache_resource>(pipeline_cache_id)) {
    pc->load(utils::safe_handle_t{});
    DE_LOG(catalogue::log_domain::resource, flow, "engine registry: pipeline cache '{}' preloaded", pipeline_cache_id);
  }
}

template <typename RenderType, typename Bootstrap>
std::unique_ptr<RenderType> make_standard_render(Bootstrap& boot, std::string app_name) {
  if (!boot.engine.render_enabled) {
    return nullptr;
  }

  const auto render_ft = frame_time_from_fps(boot.engine.render_fps);
  const std::string pipeline_cache_id = "pipeline_cache/main";
  const std::string pipeline_cache_path = project_path(boot.settings.render.cache_folder + "/painter/pipeline_cache/main.bin");
  prepare_pipeline_cache(boot, pipeline_cache_id);

  render_system_config render_cfg;
  render_cfg.engine_registry = boot.engine_resources.get();
  render_cfg.render_config_prefix = boot.settings.render.config_folder + "/";
  render_cfg.shader_config_prefix = boot.settings.render.shader_folder + "/";
  render_cfg.cache_registry = boot.engine_resources.get();
  render_cfg.pipeline_cache_id = pipeline_cache_id;
  render_cfg.pipeline_cache_path = pipeline_cache_path;
  render_cfg.graph_name = boot.settings.render.graph;
  render_cfg.menu_graph_name = boot.settings.render.menu_graph;
  render_cfg.app_name = std::move(app_name);
  render_cfg.headless = boot.engine.headless;
  render_cfg.create_vulkan_on_init = boot.engine.headless;

  return std::make_unique<RenderType>(render_ft, std::move(render_cfg));
}

template <typename RenderType, typename AssetsType, typename SoundType, typename Bootstrap>
auto make_standard_workers(Bootstrap& boot, std::string app_name) {
  using broker_type = typename RenderType::broker_type;
  static_assert(std::is_same_v<broker_type, typename AssetsType::broker_type>);
  static_assert(std::is_same_v<broker_type, typename SoundType::broker_type>);

  worker_systems<broker_type> workers;
  const auto start_gap = [&boot](const auto& system) {
    return thread_start_gap(system.frame_time(), boot.engine.thread_start_gap_divisor);
  };

  if (boot.engine.sound_enabled) {
    auto sound = std::make_unique<SoundType>(frame_time_from_fps(boot.engine.sound_fps));
    const auto wait = start_gap(*sound);
    workers.add(std::move(sound), wait, 2);
  } else {
    DE_LOG(catalogue::log_domain::main, flow, "main: sound disabled, skipping sound subsystem");
  }

  if (auto render = make_standard_render<RenderType>(boot, std::move(app_name))) {
    const auto wait = start_gap(*render);
    workers.add(std::move(render), wait, 0);
  }

  if (boot.engine.assets_enabled) {
    auto assets = std::make_unique<AssetsType>(frame_time_from_fps(boot.engine.assets_fps));
    const auto wait = start_gap(*assets);
    workers.add(std::move(assets), wait, 1);
  } else {
    DE_LOG(catalogue::log_domain::main, flow, "main: assets disabled, skipping assets subsystem");
  }

  return workers;
}

} // namespace simul
} // namespace devils_engine

#endif
