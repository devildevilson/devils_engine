#include "render_system.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <devils_engine/input/core.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/time-utils.hpp>
#include <devils_engine/utils/safe_handle.h>

#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/auxiliary.h>
#include <devils_engine/painter/makers.h>
#include <devils_engine/painter/system_info.h>
#include <devils_engine/painter/glsl_source_file.h>
#include <devils_engine/painter/shader_source_file.h>
#include <devils_engine/demiurg/resource_system.h>

#include "messages.h"
#include "message_dispatcher.h"
#include "draw_intent.h"
#include "interpolation.h"
#include "tile_map.h"
#include "tile_batch.h"
#include "actor_simulation.h"
#include "texture_resource.h"

namespace tile_frontier {
namespace core {

using namespace devils_engine;

static uint32_t make_color(const float r, const float g, const float b, const float a) {
  const auto pack = [] (const float v) {
    return uint32_t(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return (pack(r) << 0) | (pack(g) << 8) | (pack(b) << 16) | (pack(a) << 24);
}

// Политика смешивания инстанса актёра (см. blend_traits в interpolation.h). Позиция и размер —
// непрерывные (лерп), texture/color — дискретные (снап к новейшему b). teleport-guard выключен
// (snap_dist2 == 0): актёры движутся плавно; выставь порог, чтобы варп/телепорт не «ехал через
// экран», а прыгал мгновенно.
template <>
struct blend_traits<actor_instance> {
  static constexpr float snap_dist2 = 0.0f; // 0 = guard выключен
  static actor_instance mix(const actor_instance& a, const actor_instance& b, const float t) noexcept {
    actor_instance o = b; // discrete: texture, color берём у новейшего снапшота
    if constexpr (snap_dist2 > 0.0f) {
      const glm::vec2 d = b.pos - a.pos;
      if (d.x * d.x + d.y * d.y > snap_dist2) return o; // телепорт: позицию не лерпим
    }
    o.pos  = a.pos  + (b.pos  - a.pos)  * t;
    o.size = a.size + (b.size - a.size) * t;
    return o;
  }
};

struct render_simulation_init {
  cached_message_dispatcher<command_window_recreation> window_recreation_commands;
  cached_message_dispatcher<command_window_resize> window_resizing_commands;
  cached_message_dispatcher<command_gpu_transition> gpu_transition_commands;
  cached_message_dispatcher<command_write_buffer> write_buffer_commands;
  cached_message_dispatcher<command_draw_tiles> draw_tile_commands;
  cached_message_dispatcher<command_draw_actors> draw_actor_commands;

  assets_actor* aactor = nullptr; // куда слать ack о завершении GPU-перехода

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

  uint32_t tile_pair_index = painter::INVALID_RESOURCE_SLOT;
  uint32_t actor_pair_index = painter::INVALID_RESOURCE_SLOT;
  bool instance_ready = false;
  bool device_ready = false;
  bool base_ready = false;
  bool surface_ready = false;
  bool graph_ready = false;
  bool tiles_ready = false;
  bool actors_ready = false;
  bool actor_draw_ready = false;
  snapshot_interpolator<actor_instance> actor_interp;   // prev/cur + timing + blend (см. interpolation.h)
  std::vector<uint8_t> actor_interp_bytes;              // переиспользуемый выход resolve() -> GPU
  std::chrono::steady_clock::time_point actor_draw_last_tp{}; // для РЕАЛЬНОГО wall-time между кадрами (п.①)
  bool actor_draw_tp_valid = false;

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
  c.tiles_ready = false;
  c.actors_ready = false;
  c.actor_draw_ready = false;
  c.actor_draw_tp_valid = false; // сброс wall-clock базы: после detach/shutdown не считаем гигантский dt
  c.tile_pair_index = painter::INVALID_RESOURCE_SLOT;
  c.actor_pair_index = painter::INVALID_RESOURCE_SLOT;
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
  c.tiles_ready = false;
  c.actors_ready = false;
  c.actor_draw_ready = false;
  c.actor_draw_tp_valid = false; // сброс wall-clock базы: после detach/shutdown не считаем гигантский dt
  c.tile_pair_index = painter::INVALID_RESOURCE_SLOT;
  c.actor_pair_index = painter::INVALID_RESOURCE_SLOT;
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
  c.base->get_or_create_pipeline_cache(c.config.cache_registry, c.config.pipeline_cache_id);

  if (c.config.engine_registry == nullptr) utils::error{}("render: engine registry is null (render-graph source)");
  // Фаза 3: создаём GPU-ресурсы только под активный граф (commit посчитает его used-set).
  c.base->set_startup_graph(c.config.graph_name);
  // п.7: парсинг конфига живёт СНАРУЖИ graphics_base — билдер отдаёт распарсенное описание,
  // класс только устанавливает его и создаёт GPU-ресурсы.
  auto render_cfg_data = painter::build_render_config(c.config.engine_registry, c.config.render_config_prefix);
  const auto res = c.base->commit_parsed_resources(render_cfg_data);
  if (res != 0) utils::error{}("Could not commit render config from engine registry prefix '{}'", c.config.render_config_prefix);
  // Шейдеры тоже из движкового реестра (Фаза 1): create_pipeline тянет их по shader-префиксу.
  c.base->set_shader_source(c.config.engine_registry, c.config.shader_config_prefix);

  c.assets = std::make_unique<painter::assets_base>(c.device, c.physical_device_data.handle);
  c.assets->create_fence();
  c.assets->create_allocator(c.instance);
  c.assets->create_command_buffer(c.transfer_queue, c.physical_device_data.transfer_queue);
  c.assets->set_graphics_base(c.base.get());

  c.ctx.base = c.base.get();
  c.ctx.assets = c.assets.get();
  c.base_ready = true;
}

static void render_create_tile_draw(render_simulation_init& c) {
  if (c.tiles_ready || !c.graph_ready) return;

  const uint32_t dg_index = c.base->find_draw_group("dg_tiles");
  if (dg_index == painter::INVALID_RESOURCE_SLOT) {
    utils::warn("render tiles: draw group 'dg_tiles' not found");
    return;
  }

  draw_intent<tile_instance> intent;
  if (const auto r = intent.bind(std::span<const painter::format::values>(c.base->draw_groups[dg_index].instance_layout), uint32_t(c.base->draw_groups[dg_index].stride)); !r) {
    utils::warn("render tiles: dg_tiles instance layout mismatch: {} (attr {}, expected {}, actual {})",
      core::instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
    return;
  }

  const auto quad_h = c.assets->register_buffer_storage("tile_quad");
  painter::buffer_create_info bci{ "g1", 6, 0 };
  c.assets->create_buffer_storage(quad_h, bci);

  struct vertex_data { float x, y, z; uint32_t c; };
  const uint32_t white = make_color(1.0f, 1.0f, 1.0f, 1.0f);
  const vertex_data vertices[] = {
    { -0.5f, -0.5f, 0.0f, white }, {  0.5f, -0.5f, 0.0f, white }, {  0.5f,  0.5f, 0.0f, white },
    { -0.5f, -0.5f, 0.0f, white }, {  0.5f,  0.5f, 0.0f, white }, { -0.5f,  0.5f, 0.0f, white },
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices));
  c.assets->populate_buffer_storage(quad_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(quad_h);

  c.tile_pair_index = c.base->register_pair(dg_index, quad_h, 5000);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto indi = c.base->get_current_indirect_resource_frame(c.tile_pair_index, off);
    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 6;
    cmd[0].instanceCount = 0;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }

  c.tiles_ready = true;
  utils::info("render tiles: registered tile quad draw pair");
}

static void render_create_actor_draw(render_simulation_init& c) {
  if (c.actors_ready || !c.graph_ready) return;

  const uint32_t dg_index = c.base->find_draw_group("dg_actors");
  if (dg_index == painter::INVALID_RESOURCE_SLOT) {
    utils::warn("render actors: draw group 'dg_actors' not found");
    return;
  }

  draw_intent<actor_instance> intent;
  if (const auto r = intent.bind(std::span<const painter::format::values>(c.base->draw_groups[dg_index].instance_layout), uint32_t(c.base->draw_groups[dg_index].stride)); !r) {
    utils::warn("render actors: dg_actors instance layout mismatch: {} (attr {}, expected {}, actual {})",
      core::instance_layout::match_error::to_string(r.error), r.where, r.expected, r.actual);
    return;
  }

  const auto tri_h = c.assets->register_buffer_storage("actor_triangle");
  painter::buffer_create_info bci{ "g1", 3, 0 };
  c.assets->create_buffer_storage(tri_h, bci);

  struct vertex_data { float x, y, z; uint32_t c; };
  const uint32_t white = make_color(1.0f, 1.0f, 1.0f, 1.0f);
  const vertex_data vertices[] = {
    {  0.0f,  0.58f, 0.0f, white },
    { -0.50f, -0.35f, 0.0f, white },
    {  0.50f, -0.35f, 0.0f, white },
  };

  const auto vertex_bytes = std::span(reinterpret_cast<const uint8_t*>(vertices), sizeof(vertices));
  c.assets->populate_buffer_storage(tri_h, vertex_bytes, std::span<const uint8_t>());
  c.assets->mark_ready_buffer_slot(tri_h);

  c.actor_pair_index = c.base->register_pair(dg_index, tri_h, 5000);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto indi = c.base->get_current_indirect_resource_frame(c.actor_pair_index, off);
    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 3;
    cmd[0].instanceCount = 0;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }

  c.actors_ready = true;
  utils::info(
    "render actors: registered actor triangle draw pair {} (dg '{}', layout '{}', stride {}, max {})",
    c.actor_pair_index,
    c.base->draw_groups[dg_index].name,
    c.base->draw_groups[dg_index].layout_str,
    c.base->draw_groups[dg_index].stride,
    c.base->pairs[c.actor_pair_index].max_size
  );
}

static void render_update_tile_draw(render_simulation_init& c, const command_draw_tiles& msg) {
  if (!c.tiles_ready || c.tile_pair_index == painter::INVALID_RESOURCE_SLOT) return;
  if (msg.stride != tile_batch::stride()) {
    utils::warn("render tiles: bad instance stride {}, expected {}", msg.stride, tile_batch::stride());
    return;
  }

  const auto& pair = c.base->pairs[c.tile_pair_index];
  const uint32_t count = std::min(msg.count, pair.max_size);
  const size_t bytes = std::min(msg.bytes.size(), size_t(count) * msg.stride);

  for (uint32_t off = 0; off < 2; ++off) {
    const auto inst = c.base->get_current_instance_resource_frame(c.tile_pair_index, off);
    const auto indi = c.base->get_current_indirect_resource_frame(c.tile_pair_index, off);
    if (bytes != 0) std::memcpy(static_cast<uint8_t*>(inst.mapped) + inst.sub.offset, msg.bytes.data(), bytes);

    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 6;
    cmd[0].instanceCount = count;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }
}

static void render_update_actor_draw(render_simulation_init& c) {
  if (!c.actors_ready || c.actor_pair_index == painter::INVALID_RESOURCE_SLOT) return;
  if (!c.actor_interp.has_data()) return;

  const auto& pair = c.base->pairs[c.actor_pair_index];
  c.actor_interp.resolve(c.actor_interp_bytes); // интерполяция prev->cur по alpha реальных часов
  const uint32_t count = std::min(c.actor_interp.count(), pair.max_size);
  const size_t bytes = std::min(c.actor_interp_bytes.size(), size_t(count) * sizeof(actor_instance));

  for (uint32_t off = 0; off < 2; ++off) {
    const auto inst = c.base->get_current_instance_resource_frame(c.actor_pair_index, off);
    const auto indi = c.base->get_current_indirect_resource_frame(c.actor_pair_index, off);
    if (bytes != 0) std::memcpy(static_cast<uint8_t*>(inst.mapped) + inst.sub.offset, c.actor_interp_bytes.data(), bytes);

    auto cmd = reinterpret_cast<VkDrawIndirectCommand*>(&reinterpret_cast<uint8_t*>(indi.mapped)[indi.sub.offset]);
    cmd[0].vertexCount = 3;
    cmd[0].instanceCount = count;
    cmd[0].firstVertex = 0;
    cmd[0].firstInstance = 0;
  }
}

// Мост asset-текстура → дескриптор: пишем view текстуры (assets_base.texture_slots[slot])
// в asset-текстурный binding дескриптора 'textures' во ВСЕ кадровые сеты. Под-шаг 1 — одна
// текстура (элемент массива 0). Перед записью ждём GPU (одноразовая загрузка на старте);
// TODO: для рантайм-смены текстур размазать обновление на 3 кадра вместо drain.
static void render_bind_textures(render_simulation_init& c) {
  const uint32_t di = c.base->find_descriptor("textures");
  if (di == painter::INVALID_RESOURCE_SLOT) { utils::warn("render: descriptor 'textures' not found"); return; }

  auto& d = c.base->descriptors[di];
  if (d.texture_count == 0) return;
  if (c.assets->texture_slots.empty()) return;

  render_drain(c); // гарантируем, что сеты сейчас не читаются GPU (одноразовые загрузки на старте)

  const uint32_t binding = uint32_t(d.layout.size());
  const uint32_t n = d.texture_count;
  const vk::Sampler samp(c.base->samplers[d.texture_sampler].handle);

  // texture_slots ПРЕД-АЛЛОЦИРОВАН на MAX_TEXTURE_SLOTS (assets_base), большинство слотов ещё с
  // null-view (текстуры грузятся по одной). Заполняем ВСЕ N элементов массива дескриптора: i-й =
  // texture_slots[i].view, ЕСЛИ ОН ВАЛИДЕН, иначе — первый валидный view (фолбэк). Так ни один
  // элемент не остаётся VK_NULL_HANDLE (иначе VUID-VkWriteDescriptorSet-descriptorType-02997).
  vk::ImageView fallback{};
  for (uint32_t i = 0; i < n && i < c.assets->texture_slots.size(); ++i) {
    if (c.assets->texture_slots[i].view != VK_NULL_HANDLE) { fallback = vk::ImageView(c.assets->texture_slots[i].view); break; }
  }
  if (!fallback) return; // ни одной готовой текстуры — нечего биндить (не пишем null'ы)

  std::vector<vk::DescriptorImageInfo> infos(n);
  for (uint32_t i = 0; i < n; ++i) {
    vk::ImageView v = (i < c.assets->texture_slots.size()) ? vk::ImageView(c.assets->texture_slots[i].view) : vk::ImageView{};
    if (!v) v = fallback;
    infos[i] = vk::DescriptorImageInfo(samp, v, vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  std::vector<vk::WriteDescriptorSet> writes;
  for (const auto set : d.sets) {
    if (set == VK_NULL_HANDLE) continue;
    vk::WriteDescriptorSet w;
    w.dstSet = set;
    w.dstBinding = binding;
    w.dstArrayElement = 0;
    w.descriptorCount = n;
    w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    w.pImageInfo = infos.data();
    writes.push_back(w);
  }

  vk::Device(c.device).updateDescriptorSets(writes, nullptr);
  utils::info("render: bound {} textures into descriptor 'textures' ({} sets)", c.assets->texture_slots.size(), writes.size());
}

// Контракт записи в буфер: пишем сырые байты в host-visible буфер-ресурс по имени, во ВСЕ
// per_update-копии (смену активной копии делает событие update). Аналог draw_group host_visible,
// но для произвольного буфера. Требует готового графа (ресурсы созданы).
static void render_write_buffer(render_simulation_init& c, const command_write_buffer& cmd) {
  const uint32_t ri = c.base->find_resource(cmd.buffer);
  if (ri == painter::INVALID_RESOURCE_SLOT) { utils::warn("write_buffer: resource '{}' not found", cmd.buffer); return; }

  const auto& res = c.base->resources[ri];
  const auto [frame_size, ext] = res.compute_frame_size(c.base.get()); // размер ОДНОЙ копии в байтах
  const uint32_t buffering = res.compute_buffering(c.base.get());
  const size_t n = std::min(cmd.bytes.size(), size_t(frame_size));

  for (uint32_t off = 0; off < buffering; ++off) {
    const auto bf = c.base->get_current_buffer_resource_frame(ri, off);
    if (bf.mapped == nullptr) { utils::warn("write_buffer: '{}' is not host-visible", cmd.buffer); return; }
    std::memcpy(static_cast<uint8_t*>(bf.mapped) + bf.sub.offset, cmd.bytes.data(), n);
  }
}

// п.1: загрузить/выгрузить CPU-исходники шейдеров движкового реестра (glsl + spv). Хост владеет
// lifecycle ресурсов (graphics_base их НЕ грузит — см. п.7); шейдеры живут в памяти только на
// время сборки пайплайнов. force_unload у warm_and_hot_same-ресурса возвращает его в cold
// (unload_warm освобождает текст/байты).
static void set_shader_sources_loaded(const demiurg::resource_system* reg, const bool load) {
  if (reg == nullptr) return;
  std::vector<painter::glsl_source_file*> glsl;
  reg->find<painter::glsl_source_file>("shaders", glsl);
  std::vector<painter::shader_source_file*> spv;
  reg->find<painter::shader_source_file>("shaders/spv", spv);
  const auto apply = [load](demiurg::resource_interface* r) {
    if (load) r->load(utils::safe_handle_t{});
    else      r->force_unload(utils::safe_handle_t{});
  };
  for (auto* r : glsl) apply(r);
  for (auto* r : spv)  apply(r);
  utils::info("render: shader sources {} ({} glsl + {} spv)", load ? "loaded" : "unloaded", glsl.size(), spv.size());
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
  // п.1: шейдер-исходники — «загрузить → скомпилировать → выгрузить». Нужны ТОЛЬКО на время
  // сборки пайплайнов (change_render_graph: GLSL→SPIR-V→VkShaderModule, модуль транзитен; никаких
  // ссылок на ресурс не остаётся — create_pipeline берёт их через get<> транзитно). После билда
  // освобождаем CPU-текст/байты. При будущем переключении графов (п.2/3) load-before-build повторится.
  set_shader_sources_loaded(c.config.engine_registry, true);
  c.base->change_render_graph(graph_index);
  set_shader_sources_loaded(c.config.engine_registry, false);
  c.base->dump_cache_on_disk(c.config.pipeline_cache_path);

  c.graph_ready = true;
  render_create_tile_draw(c);
  render_create_actor_draw(c);
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
  actor.add_receiver<command_gpu_transition>(&container->gpu_transition_commands.dis);
  actor.add_receiver<command_write_buffer>(&container->write_buffer_commands.dis);
  actor.add_receiver<command_draw_tiles>(&container->draw_tile_commands.dis);
  actor.add_receiver<command_draw_actors>(&container->draw_actor_commands.dis);

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

void render_simulation::update([[maybe_unused]] const size_t time) {
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

  // GPU-переходы ресурсов (warm↔hot) — их исполняет рендер на своём потоке.
  // Берём только когда GPU-ресурсы готовы; иначе команды копятся в диспетчере до готовности.
  if (container->base_ready) {
    dispatcher_consume(container->gpu_transition_commands, [this] (const auto& cmd) {
      gpu_load_context ctx{ container->assets.get(), container->base.get() };
      const utils::safe_handle_t handle(&ctx);
      if (cmd.load) {
        cmd.res->load(handle);  // warm→hot: load_warm (upload+gpu_index) — полиморфно
        // как только меш на GPU и граф готов — регистрируем его на отрисовку (только меши!)
        if (cmd.res->loading_type_id == utils::type_id<texture_resource>()) {
          // текстура на GPU — перезаполняем массив дескриптора 'textures' из texture_slots
          render_bind_textures(*container);
        }
      } else {
        cmd.res->unload(handle); // hot→warm: unload_hot
      }
      if (container->aactor != nullptr) {
        command_gpu_done done{cmd.res};
        container->aactor->send(done);
      }
    });
  }

  // Контракт записи в буферы (камера и т.п.) — нужен готовый граф (ресурсы созданы).
  if (container->graph_ready) {
    dispatcher_consume(container->write_buffer_commands, [this] (const auto& cmd) {
      render_write_buffer(*container, cmd);
    });
  }

  dispatcher_consume_last(container->draw_tile_commands, [this] (const auto& cmd) {
    if (container->graph_ready) render_update_tile_draw(*container, cmd);
  });

  bool actor_snapshot = false;
  dispatcher_consume_last(container->draw_actor_commands, [this, &actor_snapshot] (const auto& cmd) {
    if (cmd.stride != actor_batch::stride()) {
      utils::warn("render actors: bad instance stride {}, expected {}", cmd.stride, actor_batch::stride());
      return;
    }
    container->actor_interp.push(
      std::span<const uint8_t>(cmd.bytes),
      std::span<const uint32_t>(cmd.ids),
      cmd.sim_frame_time);
    container->actor_draw_ready = true;
    actor_snapshot = true;
  });

  if (container->graph_ready && container->actor_draw_ready) {
    // alpha гоним по РЕАЛЬНОМУ прошедшему времени рендер-кадра, а не по номинальному шагу (п.①).
    // На кадре прихода снапшота elapsed сброшен в push() -> не продвигаем (alpha=0 -> показываем prev).
    const auto now = std::chrono::steady_clock::now();
    if (!actor_snapshot) {
      const size_t real_dt = container->actor_draw_tp_valid
        ? size_t(std::max<int64_t>(utils::count_mcs(container->actor_draw_last_tp, now), 0))
        : 0;
      container->actor_interp.advance(real_dt);
    }
    container->actor_draw_last_tp = now;
    container->actor_draw_tp_valid = true;
    render_update_actor_draw(*container);
  }

  if (container->graph_ready && container->base->can_draw()) {
    container->base->prepare_frame();
    container->ctx.prepare();
    container->ctx.draw();
    container->base->submit_frame();
  }
}

graphics_actor* render_simulation::get_actor() { return &actor; }

void render_simulation::set_assets_actor(assets_actor* aactor) {
  if (container) container->aactor = aactor;
}

}
}
