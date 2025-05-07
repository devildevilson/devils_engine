#ifndef DEVILS_ENGINE_PAINTER_LAYOUTING_H
#define DEVILS_ENGINE_PAINTER_LAYOUTING_H

#include <cstddef>
#include <cstdint>
#include "vulkan_minimal.h"
#include "primitives.h"
#include "gtl/phmap.hpp"
#include "pipeline_create_config.h"

// так тут нужно сделать какую то удобную систему лейаутов
// в том плане что мне наверное все парочку лейаутов потребуется за всю программу
// тут нужно сделать декстриптор лейаут + иметь возможность легко делать пайлайн лейауты
// у нас довольно рано будет понятно сколько чего будет вообще в игре
// или хотя бы в этом игровом состоянии
// нам тут похоже что просто нужно задать количество этого, количество того и проч

namespace devils_engine {
namespace painter {

struct layouting : public arbitrary_data {
  struct create_info {
    uint32_t readonly_storage_buffers_count;
    uint32_t combined_image_samplers_count;
    uint32_t texture_arrays_count;
    uint32_t input_attachments_count;
    uint32_t storage_buffers_count;
    uint32_t storage_images_count;
  };

  VkDevice device;

  VkSampler immutable_linear;
  VkSampler immutable_nearest;
  VkDescriptorPool pool;
  //VkDescriptorSetLayout set_layout;
  //VkDescriptorSet set;

  //VkPipelineLayout pipeline_layout;

  gtl::flat_hash_map<std::string, VkDescriptorSetLayout> set_layouts;
  gtl::flat_hash_map<std::string, VkPipelineLayout> pipe_layouts;

  // сюда нужно передать некоторое количество чисел
  // 1) количество ридонли сторадж буферов
  // 2) количество неизменяемых семлированных картинок
  // 3) количество массивов текстур
  // 4) количество аттачментов
  // 5) количество изменяемых сторадж буферов
  // 6) количество сторадж картинок (атачменты и сторадж картинки - одно и тоже?)
  layouting(VkDevice device, const create_info &info, const descriptor_set_layouts_config_t* ds_configs, const pipeline_layouts_t* pl_configs);
  ~layouting() noexcept;

  VkDescriptorSet create_descriptor_set(const std::string_view &layout_name, const std::string &set_name) const;
  VkDescriptorSetLayout find_descriptor_set_layout(const std::string_view &layout_name) const;
  VkPipelineLayout find_pipeline_layout(const std::string_view &layout_name) const;
};

}
}

#endif