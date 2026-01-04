#ifndef DEVILS_ENGINE_PAINTER_SHADER_GURU_H
#define DEVILS_ENGINE_PAINTER_SHADER_GURU_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <reflect>
#include <glm/glm.hpp>
#include "utils/core.h"
#include "utils/type_traits.h"

#include <ranges>
namespace rv = std::ranges::views;

// цель класса: принять структуры, проверить их и составить из них хедер для glsl 
// + к этому составить данные для инпута в пиплине?
// что он принимает?
// для вершинных биндингов: основная структура, (возможно) юзерфрендли структура, название биндинга, ???
// для юниформ буфера: основная структура + название + тип шейдера + индекс в дескрипторе
// для сторадж буфера: основная структура, юзерфрендли структура, название + индекс в дескрипторе + тип шейдера (рид онли?)
// для сторадж имейдж: (тоже что и для сторадж буфера)
// для атачмента: тип + название + слот (?)

// вершинные биндинги должны еще вернуть данные собственно биндинга 

// еще нужно обработать константы (они либо float либо int)
// константы как будто можно отдельно задать вообще
// константы: тип, название, значение по умолчанию + данные для биндинга констант

namespace devils_engine {
namespace painter {

std::string replace_all(std::string str, const std::string& from, const std::string& to) {
  size_t start_pos = 0;
  while((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
  }
  return str;
}

// вот так буду передавать
struct arr_t {
  glm::vec4 a;
  glm::vec4 b;
  glm::vec4 c;
};

struct abc_t {
  glm::vec4 a;
  glm::vec4 b;
  glm::vec4 c;
  arr_t arr[];
};

// должно получится что такое
//struct arr_t {
//  vec4 a;
//  vec4 b;
//  vec4 c;
//};
//layout(std140, set = 0, binding = 1) buffer abc_t_block {
//  vec4 a;
//  vec4 b;
//  vec4 c;
//  arr_t arr[];
//} abc_block;

template <typename T>
std::string make_default_c_type() {
  std::string container = "";

  const auto struct_name = replace_all(utils::type_name<T>(), "::", "_");

  container += "struct ";
  container += struct_name + "_t";
  container += " {\n";

  T obj{};
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(obj));
    using mem_type = std::remove_cvref_t<value_type>;
    const auto member_name = reflect::member_name<I>(obj);

    std::string_view type_name = "";
    if constexpr (std::is_same_v<mem_type, glm::vec4>) {
      type_name = "vec4";
    } else if constexpr (std::is_same_v<mem_type, glm::ivec4>) {
      type_name = "ivec4";
    } else if constexpr (std::is_same_v<mem_type, glm::uvec4>) {
      type_name = "uvec4";
    } else {
      // агрегатный подтип? + у нас еще будет color_t который ракрывается в
      // vec4, надо его с чем то скомбинировать или не надо, пусть будет int'ом
      // последним
      utils::error{}("Default c struct type does not support type '{}'", utils::type_name<mem_type>());
    }

    const auto variable = "  " + std::string(type_name) + " " + member_name + "; \n";
    container += variable;
  });

  container += "};\n";
  return container;
}

enum block_type {
  shader_block_uniform,
  shader_block_storage
};

// у сторадж буфера будет как минимум основной тип + тип массива
template <typename T>
std::string make_shader_block_type(const uint32_t binding_index, const block_type t) {
  std::string layout = "";
  std::string type_def = "";

  const auto struct_name = replace_all(utils::type_name<T>(), "::", "_");

  std::string block_type_name = "uniform";
  if (t == shader_block_storage) block_type_name = "buffer";

  // read only

  T obj{};
  reflect::for_each([&](auto I) {
    using value_type = decltype(reflect::get<I>(obj));
    using mem_type = std::remove_cvref_t<value_type>;
    const auto member_name = reflect::member_name<I>(obj);

    std::string_view type_name = "";
    if constexpr (std::is_same_v<mem_type, glm::vec4>) {
      type_name = "vec4";
    } else if constexpr (std::is_same_v<mem_type, glm::ivec4>) {
      type_name = "ivec4";
    } else if constexpr (std::is_same_v<mem_type, glm::uvec4>) {
      type_name = "uvec4";
    } else if constexpr ((std::is_array_v<mem_type> || std::is_pointer_v<mem_type>) && std::is_aggregate_v<std::remove_pointer_t<mem_type>>) {
      using final_t = std::remove_pointer_t<mem_type>;
      type_def = make_default_c_type<final_t>();

      const auto type_name = replace_all(utils::type_name<mem_type>(), "::", "_");
      const auto variable = std::string(type_name) + "_t " + member_name + "[]; \n";
      layout += variable;
    } else {
      utils::error{}("Shader block type does not support type '{}'", utils::type_name<mem_type>());
    }

    if (!type_name.empty()) {
      const auto variable = std::string(type_name) + " " + member_name + "; \n";
      layout += variable;
    }
  });

  const auto container = fmt::format(
    "{}layout(std140, set = 2, binding = {}) {} {}_t_block \{ \n{} \n\} {};",
    type_def, binding_index, block_type_name, struct_name,
    layout, struct_name
  );
  return container;
}

class shader_guru {
public:
  struct vertex_input_bindings {
    // compatible with VkVertexInputBindingDescription 
    struct input_binding {
      uint32_t binding;
      uint32_t stride;
      uint32_t input_rate;
    };

    // compatible with VkVertexInputAttributeDescription  
    struct vertex_attribute {
      uint32_t location;
      uint32_t binding;
      uint32_t format;
      uint32_t offset;
    };

    std::string name;
    std::string base_structure;
    // наверн накладно слишком, я для себя то эти штуки не делал
    std::string userfriendly_structure;
    std::string userfriendly_cast;

    std::vector<input_binding> bindings;
    std::vector<vertex_attribute> attribs;
  };

  struct uniform_buffer_binding {
    std::string name;
    std::string base_structure;
    uint32_t shader_type;
    uint32_t set_index;
  };

  struct storage_buffer_binding {
    std::string name;
    std::string base_structure;
    std::string userfriendly_structure;
    std::string userfriendly_cast;
    uint32_t shader_type;
    uint32_t set_index;
  };

  // тут насколько я помню вообще тип задается строкой
  struct storage_image_binding {
    std::string name;
    std::string base_structure;
    uint32_t shader_type;
    uint32_t set_index;
  };

  struct attachment_binding {
    std::string name;
    std::string base_structure;
    uint32_t slot;
  };

  struct constants_binding {
    std::string name;
    uint32_t type;
    uint32_t slot;
    double default_value;
  };

  shader_guru(const uint32_t version);

  template <typename Main>
  void add_vertex_input(std::string name) {
    const auto upper_case_name = name | rv::transform([](auto c) { return std::toupper(c); });
    
    std::vector<input_binding> bindings;
    std::vector<vertex_attribute> attribs;

    size_t location_counter = 0;
    std::string vertex_attrib;
    const auto obj = Main{};
    reflect::for_each([&](auto I) {
      using value_type = decltype(reflect::get<I>(obj));
      using mem_type = std::remove_cvref_t<value_type>;
      const auto member_name = reflect::member_name<I>(obj);
      const size_t offset = reflect::offset_of<I>(obj);

      std::string_view type_name = "";
      if constexpr (std::is_same_v<mem_type, glm::vec4>) {
        type_name = "vec4";
      } else if constexpr (std::is_same_v<mem_type, glm::ivec4>) {
        type_name = "ivec4";
      } else if constexpr (std::is_same_v<mem_type, glm::uvec4>) {
        type_name = "uvec4";
      } else if constexpr (std::is_same_v<mem_type, uint32_t>) {
        type_name = "uint";
      } else if constexpr (std::is_same_v<mem_type, int32_t>) {
        type_name = "int";
      } else if constexpr (std::is_same_v<mem_type, float>) {
        type_name = "float";
      }

      const auto variable = fmt::format("layout(binding = {}_constant, location = {}) in {} {};\n", name, location_counter, type_name, member_name);
      vertex_attrib += variable;

      input_binding b{0, sizeof(mem_type), 0};
      bindings.push_back(b);

      // формат нужно получить из типа, например vec4 == VK_FORMAT_R32G32B32A32_SNORM
      vertex_attribute a{location_counter, 0, 0, offset};
      attribs.push_back(a);
      location_counter += 1;
    }, obj);

    vertex_input_bindings in{std::move(name), std::move(vertex_attrib), "", "", std::move(bindings), std::move(attribs)};
    vertex_inputs.push_back(std::move(in));
  }

  template <typename Main>
  void add_uniform_buffer(std::string name) {
    uniform_buffers.push_back({
      std::move(name),
      make_shader_block_type<Main>(0, shader_block_uniform),
      0, 0
    });
  }

  template <typename Main, typename Userfriendly>
  void add_storage_buffer(std::string name, uint32_t shader_type, uint32_t set_index) {
    storage_buffers.push_back({
      std::move(name),
      make_shader_block_type<Main>(0, shader_block_storage),
      shader_type, set_index
    });
  }

  template <typename Main>
  void add_storage_image(std::string name, uint32_t shader_type, uint32_t set_index) {
    storage_images.push_back({
      std::move(name), "",
      shader_type, set_index
    });
  }

  template <typename Main>
  void add_attachment(std::string name, uint32_t slot) {
    const auto upper_case_name = name | rv::transform([](auto c) { return std::toupper(c); });

    size_t location_counter = 0;
    std::string vertex_attrib;
    const auto obj = Main{};
    reflect::for_each([&](auto I) {
      using value_type = decltype(reflect::get<I>(obj));
      using mem_type = std::remove_cvref_t<value_type>;
      const auto member_name = reflect::member_name<I>(obj);
      const size_t offset = reflect::offset_of<I>(obj);

      std::string_view type_name = "";
      if constexpr (std::is_same_v<mem_type, glm::vec4>) {
        type_name = "vec4";
      } else if constexpr (std::is_same_v<mem_type, glm::ivec4>) {
        type_name = "ivec4";
      } else if constexpr (std::is_same_v<mem_type, glm::uvec4>) {
        type_name = "uvec4";
      } else if constexpr (std::is_same_v<mem_type, uint32_t>) {
        type_name = "uint";
      } else if constexpr (std::is_same_v<mem_type, int32_t>) {
        type_name = "int";
      } else if constexpr (std::is_same_v<mem_type, float>) {
        type_name = "float";
      }

      const auto variable = fmt::format("layout(location = {}) out {} {};\n", location_counter, type_name, member_name);
      vertex_attrib += variable;
      location_counter += 1;
    }, obj);

    attachment_binding a{std::move(name), std::move(vertex_attrib), slot};
    attachments.push_back(std::move(a));
  }

  template <typename Main>
  void add_constant(std::string name, uint32_t slot, double default_value) {
    
  }

  std::string compile_aux_file();
private:

  std::vector<vertex_input_bindings> vertex_inputs;
  std::vector<uniform_buffer_binding> uniform_buffers;
  std::vector<storage_buffer_binding> storage_buffers;
  std::vector<storage_image_binding> storage_images;
  std::vector<attachment_binding> attachments;
  std::vector<constants_binding> constants;
};
}
}

#endif