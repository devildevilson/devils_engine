#ifndef DEVILS_ENGINE_PAINTER_COMMON_STEPS_H
#define DEVILS_ENGINE_PAINTER_COMMON_STEPS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vulkan_minimal.h"

/*
есть еще необходимость сделать специальный step где мы обновим данные индирект буферов у инстансов...
что нужно обновить? как минимум количество вершин и индексов
*/

namespace devils_engine {
namespace painter {

struct graphics_ctx;
struct graphics_base;

class step_interface {
public:
  enum class type { invalid,
                    step,
                    execution_pass,
                    render_graph };
  enum type type;
  uint32_t super;

  step_interface() noexcept;
  step_interface(enum type type, uint32_t super) noexcept;
  virtual ~step_interface() noexcept = default;
  virtual void process(graphics_ctx*, VkCommandBuffer) const = 0;
  virtual void create_related_primitives(const graphics_base*) {}
};

class pipeline_recreation {
public:
  virtual ~pipeline_recreation() noexcept = default;
  virtual void recreate_pipeline(const graphics_base*) = 0;
};

class viewport_resizer {
public:
  virtual ~viewport_resizer() noexcept = default;
  virtual void resize_viewport(const graphics_base*, const uint32_t width, const uint32_t height) = 0;
};

class graphics_step_instance : public step_interface, public pipeline_recreation {
public:
  VkRenderPass renderpass;
  uint32_t subpass_index;
  uint32_t render_target_index;

  VkDevice device;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;

  // тут где то будет пачка indirect буферов draw_group
  //std::vector<step_interface*> steps; // просто вот так сделать?
  // эта штука должна маппиться на mesh_draw_group_pair
  // мы тут должны сказать что сейчас запускается эта пачка пар
  // пары укажем в собственно в draw_group

  graphics_step_instance() noexcept;
  graphics_step_instance(uint32_t super, VkDevice device, VkRenderPass renderpass,
                         uint32_t subpass_index, uint32_t render_target_index) noexcept;

  ~graphics_step_instance() noexcept;
  void recreate_pipeline(const graphics_base*) override;
  void create_related_primitives(const graphics_base*) override;

  void create_pipeline_layout(const graphics_base*);
  void create_pipeline(const graphics_base*);
};

class compute_step_instance : public step_interface, public pipeline_recreation {
public:
  VkDevice device;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;

  compute_step_instance() noexcept;
  compute_step_instance(uint32_t super, VkDevice device) noexcept;

  ~compute_step_instance() noexcept;
  void recreate_pipeline(const graphics_base*) override;
  void create_related_primitives(const graphics_base*) override;

  void create_pipeline_layout(const graphics_base*);
  void create_pipeline(const graphics_base*);
};

class transfer_step_instance : public step_interface {
public:
  transfer_step_instance() noexcept;
  transfer_step_instance(uint32_t super) noexcept;
};

class execution_pass_instance : public step_interface, public viewport_resizer {
public:
  //std::vector<step_interface*> steps; // тут это по идее ни к чему
  VkDevice device;
  VkRenderPass renderpass;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<uint32_t> strides;
  // размеры вьюпорта? ну и динамический ли он?
  uint32_t width, height;

  execution_pass_instance() noexcept;
  ~execution_pass_instance() noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
  void resize_viewport(const graphics_base*, const uint32_t width, const uint32_t height) override;
  void create_related_primitives(const graphics_base*) override;
  void create_render_pass(const graphics_base*);
  void create_framebuffers(const graphics_base*);
  void clear_framebuffers();

  uint32_t compute_frame_index(const graphics_base*) const;
};

// тут барьеры проставим
class subpass_next : public step_interface {
public:
  VkRenderPass renderpass;
  uint32_t index;

  subpass_next() noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

// тут барьеры проставим
class execution_pass_end_instance : public step_interface {
public:
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

struct execution_group {
  struct frame {
    VkCommandBuffer buffer;
    std::vector<VkSemaphore> wait_for; // previos execution_groups
    std::vector<uint32_t> wait_for_stages;
    std::vector<VkSemaphore> signal; // next execution_groups

    frame() noexcept;
  };

  std::vector<step_interface*> steps;
  std::vector<frame> frames;

  VkDevice device;
  VkCommandPool pool;

  execution_group() noexcept;
  ~execution_group() noexcept;
  void process(graphics_ctx*) const;
  void populate_command_buffers();
};

// как минимум эта штука должна сгенерить 2 VkSubmitInfo
// во второй попадет blit изображения в текущий свопчеин
class render_graph_instance : public step_interface, public pipeline_recreation, public viewport_resizer {
public:
  struct semaphore {
    static constexpr size_t max_frames_in_flight = 8;

    std::string name;
    std::array<VkSemaphore, max_frames_in_flight> handles;

    semaphore() noexcept;
  };

  VkDevice device;

  std::vector<std::unique_ptr<step_interface>> steps;
  std::vector<execution_group> groups;

  std::vector<pipeline_recreation*> pipeline_steps;
  std::vector<viewport_resizer*> viewport_steps;

  // тут должна быть семафора конца кадра
  std::vector<semaphore> local_semaphores;

  void process(graphics_ctx*, VkCommandBuffer) const override;
  void recreate_pipeline(const graphics_base*) override;
  void resize_viewport(const graphics_base*, const uint32_t width, const uint32_t height) override;
  void clear();
  void submit(const graphics_base*, VkQueue q, VkSemaphore finish, VkFence f) const;

  uint32_t create_semaphore(std::string name, const uint32_t count);
  uint32_t find_semaphore(const std::string_view& name) const;
};

// по типу создадим подходящую команду
// откуда мы берем меш? меш должен быть доступен в таблице на момент запуска
// этих функций
class graphics_draw : public graphics_step_instance {
public:
  graphics_draw(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class graphics_draw_indexed : public graphics_step_instance {
public:
  graphics_draw_indexed(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class graphics_draw_constant : public graphics_step_instance {
public:
  graphics_draw_constant(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class graphics_draw_indexed_constant : public graphics_step_instance {
public:
  graphics_draw_indexed_constant(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class graphics_draw_indirect : public graphics_step_instance {
public:
  graphics_draw_indirect(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class graphics_draw_indexed_indirect : public graphics_step_instance {
public:
  graphics_draw_indexed_indirect(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

// Шаг отрисовки интерфейса (visage/Nuklear). НЕ использует draw_group/пары: vertex/index/command
// буферы приходят как обычные per_update host-visible ресурсы рендер-графа (cmd_params.resources
// 0/1/2 = vertices/indices/commands). Буфер команд самоописывающийся ([uint32 count][команды]),
// читается на CPU (mapped); по каждой команде: setScissor + push (tex_id,mode) + drawIndexed.
class graphics_draw_ui : public graphics_step_instance {
public:
  graphics_draw_ui(const uint32_t super, VkDevice device, VkRenderPass renderpass, const uint32_t subpass_index, const uint32_t render_target_index) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class compute_dispatch_constant : public compute_step_instance {
public:
  compute_dispatch_constant(const uint32_t super, VkDevice device) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class compute_dispatch_indirect : public compute_step_instance {
public:
  compute_dispatch_indirect(const uint32_t super, VkDevice device) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_copy_buffer : public transfer_step_instance {
public:
  transfer_copy_buffer(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_copy_image : public transfer_step_instance {
public:
  transfer_copy_image(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_copy_buffer_image : public transfer_step_instance {
public:
  transfer_copy_buffer_image(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_copy_image_buffer : public transfer_step_instance {
public:
  transfer_copy_image_buffer(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_blit_linear : public transfer_step_instance {
public:
  transfer_blit_linear(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_blit_nearest : public transfer_step_instance {
public:
  transfer_blit_nearest(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_clear_color : public transfer_step_instance {
public:
  transfer_clear_color(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

class transfer_clear_depth : public transfer_step_instance {
public:
  transfer_clear_depth(const uint32_t super) noexcept;
  void process(graphics_ctx*, VkCommandBuffer) const override;
};

} // namespace painter
} // namespace devils_engine

#endif
