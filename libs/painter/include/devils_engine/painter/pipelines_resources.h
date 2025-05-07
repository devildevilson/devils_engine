#ifndef DEVILS_ENGINE_PAINTER_PIPELINES_RESOURCES_H
#define DEVILS_ENGINE_PAINTER_PIPELINES_RESOURCES_H

#include <cstddef>
#include <cstdint>
#include "primitives.h"
#include "pipeline_create_config.h"

namespace devils_engine {
namespace demiurg {
  class resource_interface;
  class resource_system;
}

namespace painter {

// в отличие от других вещей это полноценный ресурс
class simple_graphics_pipeline : public recompile_shaders_target, public pipeline_provider {
public:
  simple_graphics_pipeline(VkDevice device, VkPipelineLayout pipeline_layout, VkPipelineCache cache, const demiurg::resource_system* system);
  ~simple_graphics_pipeline() noexcept;

  void init(VkRenderPass render_pass, const uint32_t subpass, const graphics_pipeline_create_config* conf, const size_t attachments_count, const subpass_data_t::attachment* atts);

  // КОЛОР БЛЕНДИНГ ЗАВИСИТ ОТ РАСПОЛОЖЕНИЯ АТТАЧМЕНТОВ В САБПАССЕ
  // имеет смысл это переназвать в recreate_pipeline
  // так а сами шейдеры брать из ресурсов
  uint32_t recompile_shaders() override;
protected:
  VkDevice device;
  VkPipelineCache cache;
  const demiurg::resource_system* system;

  VkRenderPass render_pass; 
  uint32_t subpass; 
  const graphics_pipeline_create_config* conf;
  size_t attachments_count;
  const subpass_data_t::attachment* atts;
};

class simple_compute_pipeline : public recompile_shaders_target, public pipeline_provider {
public:
  simple_compute_pipeline(VkDevice device, VkPipelineLayout pipeline_layout, VkPipelineCache cache, const demiurg::resource_system* system);
  ~simple_compute_pipeline() noexcept;

  void init(const compute_pipeline_create_config* conf);

  uint32_t recompile_shaders() override;
protected:
  VkDevice device;
  VkPipelineCache cache;
  const demiurg::resource_system* system;
  const compute_pipeline_create_config* conf;
};

}
}

#endif