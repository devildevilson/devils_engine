#ifndef DEVILS_ENGINE_PAINTER_RENDER_PASS_RESOURCES_H
#define DEVILS_ENGINE_PAINTER_RENDER_PASS_RESOURCES_H

#include <cstddef>
#include <cstdint>
#include "primitives.h"
#include "pipeline_create_config.h"

namespace devils_engine {
namespace painter {

enum class attachment_operation {
  dont_care,
  clear,
  keep
};

class simple_render_pass : public arbitrary_data, public render_pass_provider {
public:
  using load_ops = std::vector<attachment_operation>;
  using store_ops = std::vector<attachment_operation>;

  simple_render_pass(VkDevice device, const render_pass_data_t* create_data, const attachments_provider* provider);
  ~simple_render_pass() noexcept;

  virtual void create_render_pass();
  virtual void create_render_pass_raw(const load_ops &load, const store_ops &store);
protected:
  VkDevice device;
  const render_pass_data_t* create_data;
  const attachments_provider* provider;
};

class main_render_pass : public simple_render_pass {
public:
  main_render_pass(VkDevice device, const render_pass_data_t* create_data, const attachments_provider* attachments);
  ~main_render_pass() noexcept;

  // чистим в начале
  void create_render_pass() override;
};



}
}

#endif