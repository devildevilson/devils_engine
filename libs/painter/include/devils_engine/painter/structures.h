#ifndef DEVILS_ENGINE_PAINTER_STRUCTURES_H
#define DEVILS_ENGINE_PAINTER_STRUCTURES_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>
#include <array>
#include <tuple>

#include "common.h"
#include "vulkan_minimal.h"

namespace devils_engine {
namespace painter {

#ifndef _NDEBUG
#define DS_ASSERT_ARRAY_GET(arr, index) (arr)[(index)]; if (index >= (arr).size()) utils::error{}("Assert failed: "#arr".size() ({}) < index {}. {} {}", (arr).size(), (index), __FILE__, __LINE__);
#else
#define DS_ASSERT_ARRAY_GET(arr, index) (arr)[(index)];
#endif

struct graphics_base;
constexpr size_t INDIRECT_BUFFER_SIZE = sizeof(std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>) * 2;

struct constant_value {
  using value_t = std::tuple<uint32_t, uint32_t, uint32_t>;
  using scale_t = std::tuple<float, float, float>;

  std::string name;
  enum value_type::values type;
  value_t value;
  scale_t scale;
  uint32_t presets_count;
  uint32_t scale_presets_count;
  std::array<std::pair<preset::values, value_t>, preset::count> presets;
  std::array<std::pair<preset::values, scale_t>, preset::count> scale_presets;

  value_t current_value;
  scale_t current_scale;

  constant_value() noexcept;
  value_t compute_value() const;
  size_t reduce_value() const;
};

// атомарный? нет, синхронизация должна быть иной
// мы увеличим паралельный счетчик и заберем его в начале кадра
// используем counter и для смены картинки в свопчейне
struct counter {
  std::string name;
  std::atomic<uint32_t> value;
  std::atomic<uint32_t> next_value;

  counter() noexcept;
  counter(const counter& copy) noexcept;
  counter(counter&& move) noexcept;
  counter& operator=(const counter& copy) noexcept;
  counter& operator=(counter&& move) noexcept;

  void push_value() noexcept;
  uint32_t get_value() const noexcept;
  void inc_next_value() noexcept;
};

struct extent { uint32_t x,y,z; };

// нужно добавить тип памяти при аллокации
struct resource_container {
  std::string name;
  VmaAllocation alloc;
  size_t handle;
  struct { uint32_t x,y; } extent;
  size_t size;

  uint32_t format;
  uint32_t layers; // укажем что у буферов нет слоев
  uint32_t mips;
  uint32_t usage_mask;
  // мультисемплинг?

  void* mem_ptr;

  inline bool is_image() const noexcept { return layers >= 1; }
  inline bool host_visible() const noexcept { return mem_ptr != nullptr; }
  void create_container(VmaAllocator alc, const uint32_t host_visible);
};

struct subresource_image {
  uint32_t aspect_mask;
  uint32_t base_mip_level;
  uint32_t level_count;
  uint32_t base_array_layer;
  uint32_t layer_count;
};

struct subresource_buffer {
  size_t offset;
  size_t size;
};

struct resource {
  struct frame {
    size_t index;
    VkImageView view;

    union {
      subresource_image subimage;
      subresource_buffer subbuffer;
    };

    frame() noexcept;
  };

  std::string name;
  std::string format;
  uint32_t format_hint; // VkFormat
  uint32_t size_hint; // 1 data element size
  uint32_t size; // constant_value index
  enum role::values role;
  enum type::values type;
  uint32_t swap; // counter index
  uint32_t usage_mask;

  std::array<frame, MAX_FRAMES_IN_FLIGHT> handles;

  resource() noexcept;
  std::tuple<size_t, std::tuple<uint32_t, uint32_t>> compute_frame_size(const graphics_base* base) const;
  size_t compute_size(const graphics_base* base) const;
  uint32_t compute_buffering(const graphics_base* base) const;
};

struct resource_instance {
  std::string_view name;
  enum role::values role;
  size_t handle; // size, offset
  VkImageView view; 

  resource_instance() noexcept;
};

struct constant {
  std::string name;
  std::string layout_str;
  std::vector<uint32_t> layout;
  std::vector<double> value; // double -> [float,uint] по формату

  size_t size;
  size_t offset;

  constant() noexcept;
};

struct blend_data {
  bool enable;
  uint32_t srcColorBlendFactor;
  uint32_t dstColorBlendFactor;
  uint32_t colorBlendOp;
  uint32_t srcAlphaBlendFactor;
  uint32_t dstAlphaBlendFactor;
  uint32_t alphaBlendOp;
  uint32_t colorWriteMask;

  blend_data() noexcept;
};

// clear values?
struct render_target {
  std::string name;
  std::vector<std::tuple<uint32_t, usage::values>> resources;
  std::vector<blend_data> default_blending;

  uint32_t resource_index(const uint32_t res_id) const;
};

struct descriptor {
  std::string name;
  std::vector<std::tuple<uint32_t, enum usage::values>> layout; // slot + usage + shaders? + samplers?
  VkDescriptorSetLayout setlayout;
  std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets; // per_frame

  descriptor() noexcept;
};

struct material {
  struct shaders {
    std::string vertex;
    std::string tesselation_control;
    std::string tesselation_evaluation;
    std::string geometry;
    std::string fragment;
    std::string compute;
  };

  struct raster {
    bool depth_clamp;
    bool raster_discard;
    bool depth_bias;
    uint32_t polygon;
    uint32_t cull;
    uint32_t front_face;
    float bias_constant;
    float bias_clamp;
    float bias_slope;
    float line_width;
  };

  struct depth {
    struct stencil_op_state {
      uint32_t fail_op;
      uint32_t pass_op;
      uint32_t depth_fail_op;
      uint32_t compare_op;
      uint32_t compare_mask;
      uint32_t write_mask;
      uint32_t reference;
    };

    bool test;
    bool write;
    bool bounds_test;
    bool stencil_test;
    uint32_t compare;
    stencil_op_state front;
    stencil_op_state back;
    float min_bounds;
    float max_bounds;
  };

  std::string name;
  struct shaders shaders;
  struct raster raster;
  struct depth depth;
  
  // вьюпорт? было бы неплохо задать его для шедоу мап

  material() noexcept;
};

struct geometry {
  enum class index_type { none, u32, u16, u8 };

  std::string name;
  std::string layout_str;
  std::vector<uint32_t> vertex_layout;
  enum index_type index_type;
  uint32_t topology_type;
  bool restart;

  size_t stride;

  geometry() noexcept;
  size_t index_size() const;
};

struct draw_group {
  enum class type { device_local, host_visible };

  std::string name;
  std::string layout_str;
  std::vector<uint32_t> instance_layout;
  uint32_t budget_constant;
  uint32_t types_constant;
  enum type type; // host_visible - хост напрямую пишет сюда
  
  // пойдем со стороны большой таблицы соответствия 
  // то есть mesh_draw_group_pair вернет индекс indirect_buffer внутри draw_group
  uint32_t instances_buffer;
  uint32_t indirect_buffer;
  uint32_t descriptor;

  size_t stride;

  // у драв группы есть 2 режима работы: 
  // host visible и gpu local
  // host visible - позволяет писать напрямую в draw_group, буферы меняются per_update
  // gpu local - draw_group заполняется из ГПУ шейдера, буферы меняются per_frame

  std::vector<uint32_t> pairs; // сверху должны лежать данные с бОльшим оффсетом

  draw_group() noexcept;
};

// мы можем отсортировать по оффсетам, точнее нам желательно так сделать
struct mesh_draw_group_pair {
  std::string draw_group_name; // revalidate

  uint32_t mesh;
  uint32_t draw_group;
  uint32_t max_size;
  uint32_t indirect_offset;
  uint32_t instance_offset;

  inline mesh_draw_group_pair() noexcept : mesh(UINT32_MAX), draw_group(UINT32_MAX), max_size(0), indirect_offset(0), instance_offset(0) {}
};

// как распарсить? парсим мы в структуру и в общем то способ один и порядок тоже
struct command_params {
  command::values type;
  std::array<std::tuple<uint32_t, usage::values>, 4> resources;
  std::array<uint32_t, 4> constants;

  command_params() noexcept;
};

// как то отдельно зарегаем ui step
// там будет все очень похожим образом выглядеть, но в качестве команды будет
// draw ui 
// еще вещи которые довольно сильно отличаются это
// Post-process / Fullscreen effects - но тут compute шаг с константным dispatch
// Compute-driven системы (партиклы) - тут могут быть сгенерированы вершинные буферы (хотя может и нет...)
// Editor-only rendering - наверное идет поверх проекта
// debug - рисовать линии, оси, боксы и проч, наверное очень похожи на обычные шаги...
struct step_base {
  std::string name;
  std::vector<std::tuple<uint32_t, blend_data>> blending;
  std::vector<std::tuple<uint32_t, usage::values>> barriers;
  std::vector<uint32_t> sets;
  std::vector<uint32_t> push_constants;
  // эта команда подскажет какой instance мы создаем
  // например draw ui - нам нужен инстанс 
  // который подхватит данные из nuklear и правильно их интерпретирует
  // это пока что не объясняет откуда мы возьмем вершинные и индексные буферы
  // записать их в команду? довольно элегантно... что с драв группой?
  // ее наверное тут и не будет, команды отрисовки? подтянем в нужное место с помощью ивента
  // да UI хорошо ложится в такую штуку
  std::string command;
  uint32_t descriptor; // local
  uint32_t material;
  uint32_t geometry;
  uint32_t draw_group;

  command_params cmd_params;

  resource_usage_t read; // в инстансы? там они особо не нужны
  resource_usage_t write;

  inline step_base() noexcept : descriptor(INVALID_RESOURCE_SLOT), material(INVALID_RESOURCE_SLOT), geometry(INVALID_RESOURCE_SLOT), draw_group(INVALID_RESOURCE_SLOT) {}
  step_type::values type() const;
};

// как понять что за пасс передо мной?
// по идее по типу шагов: все шаги графические - то это графика, ну и так далее
struct execution_pass_base {
  struct resource_info {
    uint32_t slot;
    usage::values usage;
    store_op::values action;

    resource_info() noexcept;
    resource_info(const uint32_t slot, const usage::values usage, const store_op::values action) noexcept;
  };

  // wait_for, signal - но по идее это не тут должно быть указано 
  // а должно быть указано в render_graph_base
  // но у меня пока render_graph_base и эта структуры связаны

  std::string name;
  std::vector<std::string> wait_for; // семафоры
  std::vector<std::string> signal;   // семафоры
  std::vector<std::vector<resource_info>> subpasses;
  std::vector<std::vector<resource_info>> barriers;
  std::vector<uint32_t> steps;
  std::string command; // ?
  uint32_t render_target;

  std::bitset<8> step_mask; // poor man pWaitDstStageMask

  resource_usage_t read; // сборная солянка всех steps
  resource_usage_t write;

  inline execution_pass_base() noexcept : render_target(INVALID_RESOURCE_SLOT) {}
  inline bool is_graphics_pass() const noexcept { return render_target != INVALID_RESOURCE_SLOT; }
  inline bool has_step_type(const step_type::values t) const noexcept { return step_mask.test(static_cast<uint32_t>(t)); }
  inline void set_step_type(const step_type::values t) noexcept { step_mask.set(static_cast<uint32_t>(t), true); }
};

// наверное мы при переходе от render_graph к render_graph
// почистим все предыдущие step_interface
// мы должны еще выгрузить все ресурсы
struct render_graph_base {
  std::string name;
  std::vector<uint32_t> passes;
  uint32_t present_source;

  resource_usage_t read;
  resource_usage_t write;

  inline render_graph_base() noexcept : present_source(UINT32_MAX) {}
};

struct semaphore {
  std::string name;
  std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> handles;

  semaphore() noexcept;
};

// ничего не создаем, только парсим и связываем структуры друг с другом
// передаем локальный graphics_base, потому что отдельную структуру создавать капец лень
void parse_data(graphics_base* ctx, std::string path); // folder?
command_params parse_command(graphics_base* ctx, const uint32_t step_index, const std::string_view& command_str);

}
}

#endif