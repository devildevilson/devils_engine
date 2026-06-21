#include "render_system.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>

#include <devils_engine/input/core.h>
#include <devils_engine/utils/core.h>

#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/makers.h>
#include <devils_engine/painter/system_info.h>

#include "messages.h"
#include "message_dispatcher.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

static uint32_t make_color(const float r, const float g, const float b, const float a) {
  const auto pack = [] (const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return (pack(r) << 0) | (pack(g) << 8) | (pack(b) << 16) | (pack(a) << 24);
}

struct render_simulation_init {
  cached_message_dispatcher<command_window_recreation> window_recreation_commands;
  cached_message_dispatcher<command_window_resize> window_resizing_commands;

  render_simulation_config config;

  VkInstance instance = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  painter::physical_device_data physical_device_data;

  VkQueue graphics_queue = VK_NULL_HANDLE;
  VkQueue transfer_queue = VK_NULL_HANDLE;

  std::unique_ptr<painter::graphics_base> base;
  std::unique_ptr<painter::assets_base> assets;
  painter::graphics_ctx ctx;

  uint32_t triangle_pair_index = painter::INVALID_RESOURCE_SLOT;
  bool instance_ready = false;
  bool device_ready = false;
  bool base_ready = false;
  bool surface_ready = false;
  bool graph_ready = false;
  bool triangles_ready = false;

  ~render_simulation_init() noexcept {
    // Best-effort fallback. Normal shutdown should go through render_shutdown().
    if (device != VK_NULL_HANDLE) vk::Device(device).waitIdle();
    ctx = painter::graphics_ctx{};
    assets.reset();
    base.reset();
    if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) vk::Instance(instance).destroy(surface);
    if (device != VK_NULL_HANDLE) vk::Device(device).destroy();
    if (instance != VK_NULL_HANDLE && debug_messenger != VK_NULL_HANDLE) painter::destroy_debug_messenger(instance, debug_messenger);
    if (instance != VK_NULL_HANDLE) vk::Instance(instance).destroy();
  }
};

static void render_drain(render_simulation_init& c) {
  if (c.device == VK_NULL_HANDLE) return;

  painter::load_dispatcher3(c.device);
  if (c.base) c.base->wait_all_fences();
  if (c.graphics_queue != VK_NULL_HANDLE) vk::Queue(c.graphics_queue).waitIdle();
}

static void render_destroy_swapchain(render_simulation_init& c) {
  if (!c.base || c.base->swapchain == VK_NULL_HANDLE) return;

  render_drain(c);

  vk::Device dev(c.device);
  if (c.base->swapchain_slot != painter::INVALID_RESOURCE_SLOT) {
    auto& res = DS_ASSERT_ARRAY_GET(c.base->resources, c.base->swapchain_slot);
    for (size_t i = 0; i < c.base->swapchain_images.size(); ++i) {
      if (res.handles[i].view != VK_NULL_HANDLE) {
        dev.destroy(res.handles[i].view);
        res.handles[i].view = VK_NULL_HANDLE;
      }
    }
  }

  c.base->swapchain_images.clear();
  dev.destroy(c.base->swapchain);
  c.base->swapchain = VK_NULL_HANDLE;
}

static void render_detach_window(render_simulation_init& c) {
  render_destroy_swapchain(c);

  if (c.instance != VK_NULL_HANDLE && c.surface != VK_NULL_HANDLE) {
    vk::Instance(c.instance).destroy(c.surface);
    c.surface = VK_NULL_HANDLE;
  }

  if (c.base) c.base->surface = VK_NULL_HANDLE;
  c.surface_ready = false;
  c.graph_ready = false;
  c.triangles_ready = false;
}

static void render_shutdown(render_simulation_init& c) {
  render_drain(c);

  c.ctx = painter::graphics_ctx{};
  c.assets.reset();
  c.base.reset();

  if (c.instance != VK_NULL_HANDLE && c.surface != VK_NULL_HANDLE) {
    vk::Instance(c.instance).destroy(c.surface);
    c.surface = VK_NULL_HANDLE;
  }

  if (c.device != VK_NULL_HANDLE) {
    vk::Device(c.device).destroy();
    c.device = VK_NULL_HANDLE;
  }

  if (c.instance != VK_NULL_HANDLE) {
    if (c.debug_messenger != VK_NULL_HANDLE) {
      painter::destroy_debug_messenger(c.instance, c.debug_messenger);
      c.debug_messenger = VK_NULL_HANDLE;
    }

    vk::Instance(c.instance).destroy();
    c.instance = VK_NULL_HANDLE;
  }

  c.graphics_queue = VK_NULL_HANDLE;
  c.transfer_queue = VK_NULL_HANDLE;
  c.physical_device_data = painter::physical_device_data{};
  c.instance_ready = false;
  c.device_ready = false;
  c.base_ready = false;
  c.surface_ready = false;
  c.graph_ready = false;
  c.triangles_ready = false;
}

static void render_create_instance(render_simulation_init& c) {
  if (c.instance_ready) return;

  painter::load_dispatcher1(!c.config.headless);

  vk::ApplicationInfo ai{};
  ai.pApplicationName = "tile_frontier";
  ai.applicationVersion = VK_MAKE_VERSION(0, 1, 1);
  ai.pEngineName = "devils_engine";
  ai.engineVersion = VK_MAKE_VERSION(0, 1, 1);
  ai.apiVersion = VK_API_VERSION_1_0;

  std::vector<const char*> exts = painter::get_required_extensions(!c.config.headless);
  vk::InstanceCreateInfo ici{};
  ici.pApplicationInfo = &ai;
  assert(enable_validation_layers);
  if (::enable_validation_layers) {
    if (!painter::check_validation_layer_support(painter::default_validation_layers)) {
      utils::error{}("Requested Vulkan validation layers are not available");
    }

    ici.enabledLayerCount = painter::default_validation_layers.size();
    ici.ppEnabledLayerNames = painter::default_validation_layers.data();
  }
  ici.enabledExtensionCount = exts.size();
  ici.ppEnabledExtensionNames = exts.data();

  utils::info("tile_frontier: creating Vulkan instance");
  c.instance = vk::createInstance(ici);
  utils::info("tile_frontier: Vulkan instance created");
  painter::load_dispatcher2(c.instance);
  c.debug_messenger = painter::create_debug_messenger(c.instance);
  c.instance_ready = true;
}

static void render_create_device(render_simulation_init& c, const command_window_recreation* window = nullptr) {
  if (c.device_ready) return;

  bool cached = painter::system_info::try_load_cached_data(c.instance, &c.physical_device_data, nullptr);
  if (!cached) {
    if (c.config.headless) {
      painter::system_info si(c.instance);
      c.physical_device_data = si.choose_physical_device_headless();
    } else {
      if (window == nullptr || window->w == nullptr) {
        utils::warn("Render device cache is missing and no window surface is available; device creation is postponed");
        return;
      }

      painter::system_info si(c.instance);
      si.check_devices_surface_capability(c.surface);
      c.physical_device_data = si.choose_physical_device();
      si.dump_cache_to_disk(c.physical_device_data.handle, nullptr);
    }
  }

  painter::system_info::print_choosed_device(c.physical_device_data.handle);

  painter::device_maker dm(c.instance);
  dm.beginDevice(c.physical_device_data.handle);
  dm.createQueues(1);
  dm.features(vk::PhysicalDevice(c.physical_device_data.handle).getFeatures());
  if (c.config.headless) {
    dm.setExtensions({});
  } else {
    dm.setExtensions(painter::default_device_extensions);
  }
  c.device = dm.create({}, "tile_frontier.device");
  painter::load_dispatcher3(c.device);

  vk::Device dev(c.device);
  c.graphics_queue = dev.getQueue(c.physical_device_data.graphics_queue, 0);
  c.transfer_queue = dev.getQueue(c.physical_device_data.transfer_queue, 0);
  painter::set_name(dev, vk::Queue(c.graphics_queue), "tile_frontier.graphics_queue");

  c.device_ready = true;
}

static void render_create_base_resources(render_simulation_init& c) {
  if (c.base_ready || !c.device_ready) return;

  c.base = std::make_unique<painter::graphics_base>(
    c.instance,
    c.device,
    c.physical_device_data.handle,
    c.config.headless ? painter::presentation_engine_type::no_present : painter::presentation_engine_type::main
  );

  c.base->create_allocator();
  c.base->create_command_pool(c.physical_device_data.graphics_queue, c.graphics_queue);
  c.base->create_descriptor_pool();
  c.base->get_or_create_pipeline_cache(c.config.pipeline_cache_path);

  const auto res = c.base->recreate_basic_resources(c.config.render_config_folder);
  if (res != 0) utils::error{}("Could not parse render config folder '{}'", c.config.render_config_folder);

  c.assets = std::make_unique<painter::assets_base>(c.device, c.physical_device_data.handle);
  c.assets->create_fence();
  c.assets->create_allocator(c.instance);
  c.assets->create_command_buffer(c.transfer_queue, c.physical_device_data.transfer_queue);
  c.assets->set_graphics_base(c.base.get());

  c.ctx.base = c.base.get();
  c.ctx.assets = c.assets.get();
  c.base_ready = true;
}

static void render_create_test_triangles(render_simulation_init& c) {
  if (c.triangles_ready || !c.graph_ready) return;

  const auto tri_h = c.assets->register_buffer_storage("triangle");
  painter::buffer_create_info bci{ "g1", 3, 0 };
  c.assets->create_buffer_storage(tri_h, bci);

  struct buffer_data { float x, y, z; uint32_t c; };
  const buffer_data buffer_mem[] = {
    { -1.0f, -1.0f, 0.0f, make_color(1.0f, 0.0f, 0.0f, 1.0f) },
    {  1.0f, -1.0f, 0.0f, make_color(0.0f, 1.0f, 0.0f, 1.0f) },
    {  0.0f,  1.0f, 0.0f, make_color(0.0f, 0.0f, 1.0f, 1.0f) }
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(buffer_mem), sizeof(buffer_mem));
  c.assets->populate_buffer_storage(tri_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(tri_h);

  const uint32_t dg_index = c.base->find_draw_group("dg1");
  if (dg_index == painter::INVALID_RESOURCE_SLOT) utils::error{}("Could not find draw group 'dg1'");

  c.triangle_pair_index = c.base->register_pair(dg_index, tri_h, 500);

  const auto inst = c.base->get_current_instance_resource_frame(c.triangle_pair_index, 1);
  const auto indi = c.base->get_current_indirect_resource_frame(c.triangle_pair_index, 1);

  struct vec4 { float x, y, z, w; };
  auto ptr = reinterpret_cast<vec4*>(&reinterpret_cast<uint8_t*>(inst.mapped)[inst.sub.offset]);
  auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);

  ptr[0] = vec4{  0.0f,  0.5f, 0.0f, 0.0f };
  ptr[1] = vec4{  0.5f,  0.0f, 0.0f, 0.0f };
  ptr[2] = vec4{  0.0f, -0.5f, 0.0f, 0.0f };
  ptr[3] = vec4{ -0.5f,  0.0f, 0.0f, 0.0f };

  cmd[0].vertexCount = 3;
  cmd[0].instanceCount = 4;
  cmd[0].firstVertex = 0;
  cmd[0].firstInstance = 0;

  c.base->update_event();
  c.triangles_ready = true;
}

static void render_attach_window(render_simulation_init& c, const command_window_recreation& cmd) {
  if (c.config.headless) return;
  if (!c.instance_ready) render_create_instance(c);

  if (c.surface != VK_NULL_HANDLE) render_detach_window(c);

  const auto res = input::create_window_surface(c.instance, cmd.w, nullptr, &c.surface);
  if (res != static_cast<uint32_t>(vk::Result::eSuccess)) {
    utils::error{}("Could not create window surface, got {}", vk::to_string(static_cast<vk::Result>(res)));
  }
  c.surface_ready = true;

  render_create_device(c, &cmd);
  render_create_base_resources(c);

  c.base->set_surface(c.surface, cmd.width, cmd.height);
  c.base->resize_viewport(cmd.width, cmd.height);

  const uint32_t graph_index = c.base->find_render_graph(c.config.graph_name);
  if (graph_index == painter::INVALID_RESOURCE_SLOT) utils::error{}("Could not find render graph '{}'", c.config.graph_name);

  c.base->populate_constant_default_values();
  c.base->change_render_graph(graph_index);
  c.base->dump_cache_on_disk(c.config.pipeline_cache_path);

  c.graph_ready = true;
  render_create_test_triangles(c);
}

render_simulation::render_simulation(const size_t frame_time, render_simulation_config config) noexcept :
  simul::advancer(frame_time),
  container(std::make_unique<render_simulation_init>())
{
  container->config = std::move(config);
}

render_simulation::~render_simulation() noexcept {
  if (container) render_shutdown(*container);
}

void render_simulation::init() {
  actor.add_receiver<command_window_recreation>(&container->window_recreation_commands.dis);
  actor.add_receiver<command_window_resize>(&container->window_resizing_commands.dis);

  if (container->config.create_vulkan_on_init) {
    render_create_instance(*container);
    render_create_device(*container);
    render_create_base_resources(*container);
  }

  // еще дополнительно нужно создать менеджера GPU ресурсов
  // я вот о чем подумал: должен быть менеджер ассетов, который
  // раздаст память и займется вопросами копирования
  // + к этому сделать реестр текущих ресурсов, то есть
  // внешняя система получает id -> он ведет к менеджеру ->
  // тот подсказывает в каком состоянии находится GPU ресурс ->
  // если еще не готов, то получаем индекс ресурса по умолчанию (0) ->
  // если готов то он к этому времени окажется в binding'е, отправляем индекс в сете
  // ресурсы GPU могут находиться в нескольких состояниях
  // empty, resource_exists, resource_has_data, ready

  // тогда у менеджера ассетов задача такая: быть реестром всех возможных GPU ресурсов на текущий момент
  // он будет иногда перекидывать информацию в поток ассетов и принимать задачи по загрузке из хоста
  // чем тогда поток ассетов будет заниматься? вообще по идее его задача распарсить дерево
  // ресурсов и поработать с диском, в том числе что то наоборот спихнуть на диск
}

bool render_simulation::stop_predicate() const { return false; }

void render_simulation::update(const size_t time) {
  // тут че вообще делаем? пробегаем события
  // заходим в рендер граф, обрабатываем
  // вообще ничего особо сложного

  // событие изменения размеров окна
  // событиe обновления данных
  // событиe обновления настроек
  // ...

  // в конце заходим в рендер граф и рисуем все подряд

  // ловим событие пересоздания окна
  dispatcher_consume_last(container->window_recreation_commands, [this] (const auto& cmd) {
    render_attach_window(*container, cmd);
  });

  // ловим событие изменение размеров окна
  dispatcher_consume_last(container->window_resizing_commands, [this] (const auto& cmd) {
    if (container->base && container->surface_ready) container->base->resize_viewport(cmd.width, cmd.height);
  });

  if (container->triangles_ready && container->base->can_draw()) {
    container->base->prepare_frame();
    container->ctx.prepare();
    container->ctx.draw();
    container->base->submit_frame();
  }
}

graphics_actor* render_simulation::get_actor() { return &actor; }

}
}
