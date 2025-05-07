#include "draw_resource.h"

#include "glm/mat4x4.hpp"
#include "glm/vec4.hpp"
#include "glm/common.hpp"
#include "glm/ext.hpp"

#include "header.h"
#include "painter/makers.h"

namespace devils_engine {
namespace visage {
// много памяти скушает
const size_t gui_index_buffer_size = 10 * 1024 * 1024;
const size_t gui_vertex_buffer_size = 30 * 1024 * 1024;
const size_t gui_storage_buffer_size = 30 * 1024 * 1024;
const size_t gui_uniform_buffer_size = sizeof(glm::mat4) + sizeof(glm::mat4);

draw_resource::draw_resource(VkDevice device, VmaAllocator allocator, VkDescriptorSet set, nk_context* ctx, nk_buffer* cmds) :
  device(device), allocator(allocator), set(set), ctx(ctx), cmds(cmds),
  index_host(allocator, gui_index_buffer_size), vertex_host(allocator, gui_vertex_buffer_size),
  storage_host(allocator, gui_storage_buffer_size), uniform_host(allocator, gui_uniform_buffer_size),
  index_gpu(allocator, gui_index_buffer_size), vertex_gpu(allocator, gui_vertex_buffer_size),
  storage_gpu(allocator, gui_storage_buffer_size), uniform_gpu(allocator, gui_uniform_buffer_size),
  frame_allocator(gui_storage_buffer_size, sizeof(glm::vec4))
{
  interface_provider::index = index_gpu.buffer;
  interface_provider::vertex = vertex_gpu.buffer;
  interface_provider::set = set;

  painter::descriptor_set_updater dsu(device);
  dsu.currentSet(set);
  dsu.begin(0, 0, vk::DescriptorType::eUniformBuffer).buffer(uniform_gpu.buffer);
  dsu.begin(1, 0, vk::DescriptorType::eStorageBuffer).buffer(storage_gpu.buffer);
  dsu.update();
}

draw_resource::~draw_resource() noexcept {
  // ???
}

struct gui_vertex {
  glm::vec2 pos;
  glm::vec2 uv;
  uint32_t color;
};

void draw_resource::prepare(const uint32_t width, const uint32_t height) {
  memcpy(storage_host.mapped_data(), frame_allocator.data(), frame_allocator.size());
  frame_allocator.clear();

  /* fill convert configuration */
  static const struct nk_draw_vertex_layout_element vertex_layout[] = {
    {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(gui_vertex, pos)},
    {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(gui_vertex, uv)},
    {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(gui_vertex, color)},
    {NK_VERTEX_LAYOUT_END}
  };

  nk_draw_null_texture null{nk_handle{0}, nk_vec2(0,0)};
  // нужен ли мне изменяющийся антиаляисинг?
  const nk_convert_config config{
    1.0f,
    NK_ANTI_ALIASING_OFF,
    NK_ANTI_ALIASING_OFF,
    22,
    22,
    22,
    null, // нулл текстура
    vertex_layout,
    sizeof(gui_vertex), // размер вершины
    alignof(gui_vertex) // алигн вершины
  };

  nk_buffer vertices;
  nk_buffer elements;
  nk_buffer_init_fixed(&vertices, vertex_host.mapped_data(), gui_vertex_buffer_size);
  nk_buffer_init_fixed(&elements, index_host.mapped_data(), gui_index_buffer_size);

  // а откуда приходит cmds?
  const uint32_t flags = nk_convert(ctx, cmds, &vertices, &elements, &config);
  if (flags != 0) {
    if ((flags & NK_CONVERT_COMMAND_BUFFER_FULL) == NK_CONVERT_COMMAND_BUFFER_FULL) {
      utils::error("Command buffer full");
    }

    if ((flags & NK_CONVERT_VERTEX_BUFFER_FULL) == NK_CONVERT_VERTEX_BUFFER_FULL) {
      utils::error("Vertex buffer full");
    }

    if ((flags & NK_CONVERT_ELEMENT_BUFFER_FULL) == NK_CONVERT_ELEMENT_BUFFER_FULL) {
      utils::error("Index buffer full");
    }

    utils::error("Invalid data");
  }

  auto mat = reinterpret_cast<glm::mat4*>(uniform_host.mapped_data());
  *mat = glm::mat4(
    2.0f / float(width),  0.0f,  0.0f,  0.0f,
    0.0f,  2.0f / float(height),  0.0f,  0.0f,
    0.0f,  0.0f, -1.0f,  0.0f,
    -1.0f, -1.0f,  0.0f,  1.0f
  );

  const nk_draw_command* cmd = nullptr;
  nk_draw_foreach(cmd, ctx, cmds) {
    interface_piece_t command;
    command.count = cmd->elem_count;
    command.rect = { uint32_t(cmd->clip_rect.x), uint32_t(cmd->clip_rect.y), uint32_t(cmd->clip_rect.w), uint32_t(cmd->clip_rect.h) };
    command.texture_id = cmd->texture.id;
    command.userdata_id = cmd->userdata.id;

    interface_provider::cmds.push_back(command);
  }

  // вот теперь все буферы готовы и можно копировать
}
}
}