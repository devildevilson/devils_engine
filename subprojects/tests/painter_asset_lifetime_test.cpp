#include <doctest/doctest.h>

#include <devils_engine/painter/assets_base.h>
#include <devils_engine/painter/gpu_load_context.h>
#include <devils_engine/painter/gpu_texture_resource.h>
#include <devils_engine/painter/graphics_base.h>
#include <devils_engine/painter/mesh_resource.h>
#include <devils_engine/utils/safe_handle.h>

using namespace devils_engine;

TEST_CASE("painter gpu resources mark their slots for render-thread removal") {
  painter::assets_base assets(VK_NULL_HANDLE, VK_NULL_HANDLE);
  painter::graphics_base graphics(
    VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
    painter::presentation_engine_type::no_present);
  assets.set_graphics_base(&graphics);

  const auto texture_slot = assets.register_texture_storage("texture");
  assets.texture_slots[texture_slot].state = painter::asset_state::ready;
  painter::gpu_texture_resource texture;
  texture.gpu_index = texture_slot;

  const auto mesh_slot = assets.register_buffer_storage("mesh");
  assets.buffer_slots[mesh_slot].state = painter::asset_state::ready;
  painter::mesh_resource mesh;
  mesh.gpu_index = mesh_slot;

  painter::gpu_load_context ctx{&assets, &graphics};
  const utils::safe_handle_t handle(&ctx);
  texture.unload_hot(handle);
  mesh.unload_hot(handle);

  CHECK(texture.gpu_index == painter::gpu_texture_resource::invalid_gpu_index);
  CHECK(mesh.gpu_index == painter::mesh_resource::invalid_gpu_index);
  CHECK(assets.texture_slots[texture_slot].state == painter::asset_state::pending_remove);
  CHECK(assets.buffer_slots[mesh_slot].state == painter::asset_state::pending_remove);
}

TEST_CASE("painter unregister_mesh removes every draw-group pair") {
  painter::graphics_base graphics(
    VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
    painter::presentation_engine_type::no_present);
  graphics.draw_groups.resize(2);
  graphics.pairs.resize(3);

  graphics.pairs[0].draw_group = 0;
  graphics.pairs[0].mesh = 7;
  graphics.pairs[1].draw_group = 0;
  graphics.pairs[1].mesh = 8;
  graphics.pairs[2].draw_group = 1;
  graphics.pairs[2].mesh = 7;
  graphics.draw_groups[0].pairs = {0, 1};
  graphics.draw_groups[1].pairs = {2};

  graphics.unregister_mesh(7);

  CHECK(graphics.draw_groups[0].pairs == std::vector<uint32_t>{1});
  CHECK(graphics.draw_groups[1].pairs.empty());
  CHECK(graphics.pairs[0].draw_group == painter::invalid_resource_slot);
  CHECK(graphics.pairs[0].mesh == painter::invalid_resource_slot);
  CHECK(graphics.pairs[1].draw_group == 0);
  CHECK(graphics.pairs[1].mesh == 8);
  CHECK(graphics.pairs[2].draw_group == painter::invalid_resource_slot);
  CHECK(graphics.pairs[2].mesh == painter::invalid_resource_slot);
}
