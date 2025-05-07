#ifndef DEVILS_ENGINE_PAINTER_PIPELINE_CREATE_CONFIG_H
#define DEVILS_ENGINE_PAINTER_PIPELINE_CREATE_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <array>

namespace devils_engine {
namespace painter {

// в таком виде мы можем дампить это дело на диск
struct graphics_pipeline_create_config {
  // для биндингов было бы неплохо использовать названия
  // например название структуры с которой биндинг берется
  struct binding_t {
    struct attribute_t {
      uint32_t location;
      uint32_t binding;
      uint32_t format;
      uint32_t offset;
    };

    uint32_t id;
    uint32_t stride;
    std::string type;

    std::vector<attribute_t> attributes;

    // std::string bind_data_type_name; // вместо страйда и аттрибутов
  };

  struct shader_module {
    std::string type;
    //demiurg::resource_interface* resource; // укажем тут путь? думаю что здравая идея
    std::string path;
    std::string func_name;

    std::map<std::string, std::string> defines;
  };

  struct constant_data_t {
    uint32_t location;
    union { float float_data[4]; uint32_t uint_data[4]; int32_t int_data[4]; };
    std::string type;
  };

  struct stencil_op_state_t {
    uint32_t fail_op;
    uint32_t pass_op;
    uint32_t depth_fail_op;
    uint32_t compare_op;
    uint32_t compare_mask;
    uint32_t write_mask;
    uint32_t reference;
  };

  // binding ? по идее определяется типом шейдера, но в будущем поди можно указать
  // в конфиге настроечку биндингов

  std::string name;
  std::vector<shader_module> shaders;
  // константы?
  std::vector<double> constants;
  
  std::vector<binding_t> bindings;

  // вьюпорт и скиссор? кажется их можно указать динамическими и не париться

  uint32_t topology;
  bool primitive_restart;

  bool depth_clamp;
  bool rasterizer_discard; // че это?
  uint32_t polygon_mode;
  uint32_t cull_mode;
  uint32_t front_face;
  struct depth_bias_t { bool enable;  float const_factor, clamp, slope_factor; } depth_bias;
  float line_width;

  uint32_t rasterization_samples;
  struct sample_shading_t { bool enable; float min_sample_shading; std::vector<uint32_t> masks; } sample_shading;
  struct multisample_coverage_t { bool alpha_to_coverage; bool alpha_to_one; } multisample_coverage;
  
  bool depth_test;
  bool depth_write;
  uint32_t depth_compare;
  struct stencil_test_t { bool enable; stencil_op_state_t front, back; } stencil_test;
  struct depth_bounds_t { bool enable; float min_bounds, max_bounds; } depth_bounds;

  // остаток колор блендинга VkPipelineColorBlendAttachmentState мы укажем собственно в атачментах
  // по итогу указали в сабпассе... почему там? скорее нужно все таки указать тут
  // но при этом как то сопоставить сам атачмент с сабпассом и блендингом
  // я бы сказал что нужно по имени будет найти аттачмент и его в сабпассе
  // а остальным блендинги выключить
  struct color_blending_state_t { 
    struct logic_op_t { bool enable; uint32_t operation; };
    logic_op_t logic_op; 
    std::array<float,4> blend_constants; // MSVC glaze fails to understand c-style array?
  };
  color_blending_state_t color_blending_state;
};

struct compute_pipeline_create_config {
  struct shader_module {
    std::string type;
    std::string path;
    std::string func_name;

    std::unordered_map<std::string, std::string> defines;
  };

  std::string name;
  shader_module shader;
};

enum class subpass_attachment_type {
  intended,
  input,
  preserve,
  sampled,
  storage,
};

struct color_blending_t { 
  bool enable; 
  uint32_t src_color_blend_factor; 
  uint32_t dst_color_blend_factor; 
  uint32_t color_blend_op; 
  uint32_t src_alpha_blend_factor; 
  uint32_t dst_alpha_blend_factor; 
  uint32_t alpha_blend_op; 
  uint32_t color_write_mask; 
};

struct subpass_data_t {
  struct attachment {
    subpass_attachment_type type;
    color_blending_t blending;
  };

  std::string name;
  std::vector<attachment> attachments;
};

struct attachment_description_t {
  std::string initial_state;
  std::string final_state;
  std::string load_op;
  std::string store_op;
  std::string stencil_load_op;
  std::string stencil_store_op;
};

struct render_pass_data_t {
  std::string name;
  std::vector<attachment_description_t> descriptions;
  // их должно быть минимум два, последний всегда внешний
  std::vector<subpass_data_t> subpasses;
};

// сделать именные форматы и запилить возможность указывать в каком аттачменте расположен свопчеин
struct attachment_config_t {
  std::string name;
  uint32_t format;
};

struct sampler_config_t {
  std::string name;
  std::string filter_min, filter_mag;
  uint32_t mipmap_mode;
  uint32_t address_mode_u, address_mode_v, address_mode_w;
  struct anisotropy_t { bool enable; float max; } anisotropy;
  struct depth_compare_op_t { bool enable; uint32_t operation; } depth_compare_op;
  struct lod_t { float min, max, bias; } lod;
  uint32_t border_color;
  bool unnormalized_coordinates;
};

struct descriptor_set_layout_binding_t {
  uint32_t type;
  uint32_t shader_stages;
  uint32_t count;
};

using descriptor_set_layouts_config_t = std::unordered_map<std::string, std::vector<descriptor_set_layout_binding_t>>;

struct pipeline_layout_config_t {
  std::vector<std::string> set_layouts;
  uint32_t push_constant_size;
};

using pipeline_layouts_t = std::unordered_map<std::string, pipeline_layout_config_t>;

}
}

#endif