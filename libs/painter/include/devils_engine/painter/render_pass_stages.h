#ifndef DEVILS_ENGINE_PAINTER_RENDER_PASS_STAGES_H
#define DEVILS_ENGINE_PAINTER_RENDER_PASS_STAGES_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <tuple>
#include "primitives.h"
#include "pipeline_create_config.h"

// короче вот какая тема:
// аттачменты это ресурсы, которым не зачем появляться в стейджах
// рендер пасс это ресурс, который зависит от аттачментов и которому тоже незачем появлятся в стейдже
// фреймбуфер это ресурс, который зависит от аттачментов, от рендерпасса и от ИНДЕКСА картинки в аттачментах
// но НЕ от индекса текущего исполнения
// таким образом есть структура с фреймбуферами, в нее должны приходить:
// рендерпасс, от него аттачменты для которох он создавался

// надо переделать, сюда теперь просто передадим фреймбуфер провайдер

namespace devils_engine {
namespace painter {
struct attachments_container;

class render_pass_main : public sibling_stage, public parent_stage {
public:
  render_pass_main(VkDevice device, const framebuffer_provider* provider);
  ~render_pass_main() noexcept;

  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  VkDevice device;
  const framebuffer_provider* provider;
};

class next_subpass : public sibling_stage {
public:
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
};

}
}

#endif