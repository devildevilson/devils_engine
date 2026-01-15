#ifndef DEVILS_ENGINE_PAINTER_GRAPHICS_BASE_H
#define DEVILS_ENGINE_PAINTER_GRAPHICS_BASE_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <tuple>
#include "vulkan_minimal.h"
#include "structures.h"
#include "common_steps.h"
#include "devils_engine/utils/core.h"

/*
тут еще непонятно откуда брать дескриптор текстурок...
*/

namespace devils_engine {
namespace painter {

struct assets_base;
constexpr uint32_t default_frames_in_flight = 3;

// читаются painter_base, но данных больше
struct graphics_options {
  std::tuple<uint32_t, uint32_t> viewport;
  uint32_t shadow_map_opt;
  // motion blur......
};

struct vk_queue {
  VkQueue queue;
  std::mutex mutex;

  inline vk_queue(VkQueue queue) noexcept : queue(queue) {}
  void wait_idle();
  uint32_t submit();
};

enum class presentation_engine_type { main, no_present };

// не знает про окно
// это общий контекст, наверное будем везде его таскать с собой
// вообще не должен быть таковым
// наверное нас интерсует graphics_base, от которого сделаем graphics_ctx
// для других систем будет что то вроде assets_base
// и compute_base (очень похожая на graphics_base, но без рендер штук)
struct graphics_base {
  VkInstance instance;
  VkDevice device;
  VkPhysicalDevice physical_device;
  VkSurfaceKHR surface;

  // внутренние ресурсы
  VkPipelineCache cache;
  VkQueue graphics;
  VkQueue transfer;
  VkCommandPool command_pool;
  VkDescriptorPool descriptor_pool;
  VkSwapchainKHR swapchain; // для свопчейна еще будут семафоры
  std::vector<VkImage> swapchain_images;
  std::vector<VkFence> fences;

  // аллокаторы
  VmaAllocator allocator;

  std::vector<constant_value> constant_values;
  std::vector<counter> counters;
  std::vector<resource_container> resource_containers;
  std::vector<resource> resources;
  std::vector<constant> constants;
  std::vector<render_target> render_targets;
  std::vector<descriptor> descriptors;
  std::vector<material> materials;
  std::vector<geometry> geometries;
  std::vector<draw_group> draw_groups;
  
  std::vector<step_base> steps;
  std::vector<execution_pass_base> passes;
  std::vector<render_graph_base> graphs;
  // внешние семафоры
  std::vector<semaphore> semaphores;

  std::vector<mesh_draw_group_pair> pairs;

  render_graph_instance execution_graph;

  // сырые данные для constants? как мы задаем их?
  // они должны быть по умолчанию буферизированы, каунтером определим куда пишем
  std::array<std::vector<uint32_t>, 2> constants_memory;

  enum presentation_engine_type presentation_engine_type;
  uint32_t current_presentable_state;

  uint32_t swapchain_slot; // роль present
  uint32_t swapchain_counter_index;
  uint32_t per_frame_counter_index;
  uint32_t per_update_counter_index;
  uint32_t frames_in_flight_constant_value_index;
  uint32_t current_render_graph_index;

  uint32_t swapchain_image_semaphore;
  uint32_t finish_rendering_semaphore;

  uint32_t computed_current_frame_index;

  std::tuple<uint32_t, uint32_t> swapchain_image_size;

  // где то еще должен быть дексриптор для текстурок

  graphics_base(VkInstance instance, VkDevice device, VkPhysicalDevice physical_device, enum presentation_engine_type presentation_engine_type) noexcept;
  ~graphics_base() noexcept;

  // API
  void create_allocator(const size_t preferred_heap_block = 0);
  void create_command_pool(const uint32_t queue_family_index, VkQueue graphics);
  void create_descriptor_pool();
  void get_or_create_pipeline_cache(const std::string& path);
  void dump_cache_on_disk(const std::string& path) const;
  void set_surface(VkSurfaceKHR surface, const uint32_t width, const uint32_t height);
  void populate_constant_default_values();

  // придется пересоздать все зависимые ресурсы то есть render_targets и обновить дескрипторы
  void resize_viewport(const uint32_t width, const uint32_t height);
  void recreate_pipelines();
  void clear();
  // draw будет не тут наверное, а в контексте
  // внутри кадра у нас должна быть еще подготовка 
  // ну или подготовку вынести наружу
  void draw(); // обновим каунтеры, зайдем в render_graph, запишем команды, передадим их в queue
  void prepare_frame(); // чисто подготовим все что нужно со стороны graphics_base
  void submit_frame(); // тут отправим что подготовили в очередь, present когда?
  void update_frame();
  void update_event(); // обновим память + обновим per_update индекс

  bool can_draw() const;

  void change_render_graph(const uint32_t index);

  void inc_counter(const uint32_t slot);

  uint32_t compute_frame_index(const int32_t offset) const;
  uint32_t current_swapchain_image_index() const;
  uint32_t current_frame_index() const;
  uint32_t current_update_index() const;
  uint32_t frames_in_flight() const;
  uint32_t swapchain_frames() const;
  uint32_t current_frame_in_flight() const;

  std::tuple<uint32_t, uint32_t> swapchain_extent() const;

  uint32_t find_constant_value(const std::string_view& name) const;
  uint32_t find_resource(const std::string_view& name) const;
  uint32_t find_counter(const std::string_view& name) const;
  uint32_t find_constant(const std::string_view& name) const;
  uint32_t find_render_target(const std::string_view& name) const;
  uint32_t find_descriptor(const std::string_view& name) const;
  uint32_t find_material(const std::string_view& name) const;
  uint32_t find_geometry(const std::string_view& name) const;
  uint32_t find_draw_group(const std::string_view& name) const;
  uint32_t find_execution_step(const std::string_view& name) const;
  uint32_t find_execution_pass(const std::string_view& name) const;
  uint32_t find_render_graph(const std::string_view& name) const;
  uint32_t find_semaphore(const std::string_view& name) const;

  // указываем обязательно название draw_group в которую регистрируем, чтобы потом ревалидировать проще
  // хотя нафига...... соберем старые названия и попытаемся сопоставить их? ну звучит как план
  uint32_t register_pair(const uint32_t draw_group, const uint32_t mesh, const uint32_t max_count);
  void unregister_pair(const uint32_t draw_group, const uint32_t mesh);

  uint32_t find_pair(const uint32_t draw_group, const uint32_t mesh) const;

  // как заберем данные из констант?
  void* get_constant_data(const uint32_t index);
  const void* get_constant_data(const uint32_t index) const;
  template <typename T>
    requires(std::is_trivially_copyable_v<T>&& alignof(T) <= alignof(float))
  T get_constant_data(const uint32_t index) const;

  void write_constant_data(const uint32_t slot, const void* data, const size_t size);
  template <typename T>
    requires(std::is_trivially_copyable_v<T>&& alignof(T) <= alignof(float))
  void write_constant_data(const uint32_t slot, const T& data);


  int32_t recreate_basic_resources(const std::string& folder); // возвратим статус

  // PRIVATE

  // resize_viewport 
  void recreate_swapchain(const uint32_t width, const uint32_t height); // свопчеин частично состоит из ресурса который мы задали
  void recreate_screensize_resources(const uint32_t width, const uint32_t height);

  // ?
  void recreate_render_graph(); // заново прочитаем настройки, распарсим их, удалим старое, создадим новое

  // clear
  void clear_render_graph();
  void clear_resources();
  void clear_semaphores();

  void create_fences();
  void create_global_semaphores();

  // prepare_frame
  void update_counters();
  void image_acquire();
  void update_descriptors();
  void wait_fence();

  // update_event
  void update_constant_memory();

  // recreate_basic_resources
  void clear_prev_resources();
  void create_descriptor_set_layouts();
  void create_resources();
  void create_descriptor_sets();
  void revalidate_pairs(const std::vector<std::string> &prev_drav_group_names);

  void update_all_descriptors(); // ?

  template <typename T, typename... Args>
  T* create_render_step(Args&&... args);

  bool presentable_state_stable() const;
  bool presentable_state_suboptimal() const;
  bool presentable_state_waiting_host_event() const;
};

struct resource_inst {
  union {
    VkImage img;
    VkBuffer buf;
  };

  union {
    subresource_image subimg;
    subresource_buffer subbuf;
  };

  VkImageView view;
  struct { uint32_t x, y; } extent;

  role::values role;

  usage::values usage; // перезаписываем
};

struct buffer_memory_barrier {
  uint32_t stype;
  void* pNext;
  uint32_t srcAccessMask;
  uint32_t dstAccessMask;
  uint32_t srcQueueFamilyIndex;
  uint32_t dstQueueFamilyIndex;
  VkBuffer buffer;
  size_t offset;
  size_t size;
};

struct image_memory_barrier {
  uint32_t stype;
  void* pNext;
  uint32_t srcAccessMask;
  uint32_t dstAccessMask;
  uint32_t oldLayout;
  uint32_t newLayout;
  uint32_t srcQueueFamilyIndex;
  uint32_t dstQueueFamilyIndex;
  VkImage image;
  subresource_image subresourceRange;
};

struct graphics_ctx {
  const graphics_base* base;
  const assets_base* assets;

  // текущие ресурсы
  std::vector<resource_inst> resources;
  std::vector<VkDescriptorSet> descriptors;

  // кеш
  std::vector<buffer_memory_barrier> buffer_barriers;
  std::vector<image_memory_barrier> image_barriers;
  std::vector<VkDescriptorSet> descriptors_cache;

  inline graphics_ctx() noexcept : base(nullptr), assets(nullptr) {}
  void prepare();
  void draw();
};



template <typename T>
  requires(std::is_trivially_copyable_v<T>&& alignof(T) <= alignof(float))
T graphics_base::get_constant_data(const uint32_t index) const {
  const auto& c = DS_ASSERT_ARRAY_GET(constants, index);
  if (c.size < sizeof(T)) utils::error{}("Could not get type '{}' from constant '{}', size of type {} is bigger then constant size {}", utils::type_name<T>(), c.name, sizeof(T), c.size);
  auto ptr = get_constant_data(index);
  auto t_ptr = reinterpret_cast<const T*>(ptr);
  return T(*t_ptr);
}

template <typename T>
  requires(std::is_trivially_copyable_v<T> && alignof(T) <= alignof(float))
void graphics_base::write_constant_data(const uint32_t slot, const T& data) {
  write_constant_data(slot, &data, sizeof(data));
}

template <typename T, typename... Args>
T* graphics_base::create_render_step(Args&&... args) {
  auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
  ptr->create_related_primitives(this);
  auto ptr_raw = ptr.get();

  if constexpr (std::is_base_of_v<pipeline_recreation, T>) {
    execution_graph.pipeline_steps.push_back(ptr_raw);
  }

  if constexpr (std::is_base_of_v<viewport_resizer, T>) {
    execution_graph.viewport_steps.push_back(ptr_raw);
  }

  execution_graph.steps.emplace_back(std::move(ptr));
  return ptr_raw;
}

}
}

#endif