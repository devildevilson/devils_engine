#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <doctest/doctest.h>
#include <shaderc/shaderc.h>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/painter/glsl_source_file.h"
#include "devils_engine/painter/gpu_timing.h"
#include "devils_engine/painter/graphics_base.h"
#include "devils_engine/painter/render_config_source.h"
#include "devils_engine/painter/region_draw.h"
#include "devils_engine/painter/shader_crafter.h"
#include "devils_engine/painter/shader_specialization.h"
#include "devils_engine/painter/structures.h"

using namespace devils_engine;

TEST_CASE("painter GPU timestamp delta respects valid-bit wraparound [painter]") {
  CHECK(painter::gpu_timestamp_delta(100, 145, 64) == 45);
  CHECK(painter::gpu_timestamp_delta(250, 5, 8) == 11);
  CHECK(painter::gpu_timestamp_delta(10, 20, 0) == 0);
}

TEST_CASE("painter step derives resource usages from named descriptor sets [painter]") {
  const auto storage = painter::build_render_config(PAINTER_TEST_CONFIG_ROOT);
  const auto step_slot = storage.find_execution_step("draw_tiles");
  const auto camera_slot = storage.find_resource("camera_buffer");
  REQUIRE(step_slot != painter::invalid_resource_slot);
  REQUIRE(camera_slot != painter::invalid_resource_slot);

  const auto& step = storage.steps[step_slot];
  const auto usage_count = std::count_if(step.barriers.begin(), step.barriers.end(), [&](const auto& entry) {
    return entry.resource == camera_slot && entry.usage == painter::usage::uniform;
  });
  CHECK(usage_count == 1);
  CHECK(step.read.test(camera_slot));
  CHECK_FALSE(step.write.test(camera_slot));

  const auto ui_slot = storage.find_execution_step("draw_ui");
  REQUIRE(ui_slot != painter::invalid_resource_slot);
  const auto& ui_step = storage.steps[ui_slot];
  const auto ui_camera_count = std::count_if(ui_step.barriers.begin(), ui_step.barriers.end(), [&](const auto& entry) {
    return entry.resource == camera_slot;
  });
  CHECK(ui_camera_count == 1);
}

TEST_CASE("painter draw_regions keeps region commands separate from shader data [painter]") {
  const auto storage = painter::build_render_config(PAINTER_TEST_CONFIG_ROOT);
  const auto step_slot = storage.find_execution_step("draw_regions");
  const auto data_slot = storage.find_resource("region_gpu_data");
  const auto commands_slot = storage.find_resource("region_commands");
  const auto material_slot = storage.find_material("region_material");
  REQUIRE(step_slot != painter::invalid_resource_slot);
  REQUIRE(data_slot != painter::invalid_resource_slot);
  REQUIRE(commands_slot != painter::invalid_resource_slot);
  REQUIRE(material_slot != painter::invalid_resource_slot);

  const auto& step = storage.steps[step_slot];
  CHECK(step.cmd_params.type == painter::command::draw_regions);
  CHECK(std::get<0>(step.cmd_params.resources[0]) == data_slot);
  CHECK(std::get<1>(step.cmd_params.resources[0]) == painter::usage::storage_read);
  CHECK(std::get<0>(step.cmd_params.resources[1]) == commands_slot);
  CHECK(std::get<1>(step.cmd_params.resources[1]) == painter::usage::transfer_dst);
  CHECK(step.read.test(data_slot));
  CHECK(storage.materials[material_slot].raster.depth_bias);
  CHECK(storage.materials[material_slot].raster.dynamic_depth_bias);

  painter::region_draw_header header{};
  header.region_count = 4;
  header.span_count = 8;
  header.region_stride = sizeof(painter::region_draw_command);
  header.span_stride = sizeof(painter::region_draw_span);
  CHECK(header.magic == painter::region_draw_magic);
  CHECK(painter::region_draw_buffer_size(header.region_count, header.span_count) ==
        sizeof(header) + 4 * sizeof(painter::region_draw_command) + 8 * sizeof(painter::region_draw_span));
}

TEST_CASE("painter constant defaults are active before the first frame [painter]") {
  struct dispatch_command {
    uint32_t x;
    uint32_t y;
    uint32_t z;
  };

  painter::graphics_base base(
    VK_NULL_HANDLE,
    VK_NULL_HANDLE,
    VK_NULL_HANDLE,
    painter::presentation_engine_type::no_present);

  painter::constant dispatch;
  dispatch.name = "light_dispatch";
  dispatch.layout_str = "dispatch3";
  dispatch.layout = {painter::format::dispatch3};
  dispatch.value = {120.0, 68.0, 1.0};
  dispatch.size = sizeof(dispatch_command);
  dispatch.offset = 0;
  base.constants.emplace_back(std::move(dispatch));
  for (auto& memory : base.constants_memory) {
    memory.resize(sizeof(dispatch_command) / sizeof(uint32_t), 0u);
  }

  base.populate_constant_default_values();
  auto active = base.get_constant_data<dispatch_command>(0);
  CHECK(active.x == 120u);
  CHECK(active.y == 68u);
  CHECK(active.z == 1u);

  const dispatch_command replacement{4u, 5u, 6u};
  base.write_constant_data(0, replacement);
  active = base.get_constant_data<dispatch_command>(0);
  CHECK(active.x == 120u);
  CHECK(active.y == 68u);
  CHECK(active.z == 1u);

  base.update_constant_memory();
  active = base.get_constant_data<dispatch_command>(0);
  CHECK(active.x == 4u);
  CHECK(active.y == 5u);
  CHECK(active.z == 6u);
}

TEST_CASE("glsl_source_file caches prepared SPIR-V by shader stage [painter]") {
  painter::glsl_source_file shader;
  shader.memory =
    "#version 450\n"
    "layout(location = 0) in vec2 in_pos;\n"
    "void main() {\n"
    "  gl_Position = vec4(in_pos, 0.0, 1.0);\n"
    "}\n";

  std::string error;
  CHECK(shader.prepare_spirv(nullptr, shaderc_vertex_shader, &error));
  CHECK(error.empty());
  CHECK(shader.prepared(shaderc_vertex_shader));
  CHECK_FALSE(shader.spirv.empty());

  const auto first_size = shader.spirv.size();
  CHECK(shader.prepare_spirv(nullptr, shaderc_vertex_shader, &error));
  CHECK(shader.spirv.size() == first_size);

  shader.unload_warm(utils::safe_handle_t{});
  CHECK(shader.memory.empty());
  CHECK(shader.spirv.empty());
  CHECK_FALSE(shader.prepared(shaderc_vertex_shader));
}

TEST_CASE("glsl_source_file keeps material-defined shader variants separate [painter]") {
  painter::glsl_source_file shader;
  shader.memory =
    "#version 450\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "#ifdef PF01_RED\n"
    "  out_color = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "#else\n"
    "  out_color = vec4(0.0, 0.0, 1.0, 1.0);\n"
    "#endif\n"
    "}\n";

  const std::vector<painter::glsl_source_file::shader_definition> red{{"PF01_RED", "1"}};
  const std::vector<painter::glsl_source_file::shader_definition> blue{{"PF01_BLUE", "1"}};
  std::string error;
  CHECK(shader.prepare_spirv(nullptr, shaderc_fragment_shader, red, &error));
  CHECK(shader.prepare_spirv(nullptr, shaderc_fragment_shader, blue, &error));
  const auto* red_spirv = shader.prepared_spirv(shaderc_fragment_shader, red);
  const auto* blue_spirv = shader.prepared_spirv(shaderc_fragment_shader, blue);
  REQUIRE(red_spirv != nullptr);
  REQUIRE(blue_spirv != nullptr);
  CHECK(*red_spirv != *blue_spirv);
  CHECK(shader.variants.size() == 2);

  shader.unload_warm(utils::safe_handle_t{});
  CHECK(shader.variants.empty());
}

TEST_CASE("shader_crafter serves utils shared header from generated memory include [painter]") {
  painter::glsl_source_file shader;
  shader.memory =
    "#version 450\n"
    "#include <utils/shared.h>\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "  const uint id = tex_pack(ui_draw_image, 3u, true, false, sampler_linear);\n"
    "  const float v = valid_gpu_index(tex_index_of(id)) && tex_mirror_u_of(id) ? median3(0.25, 0.5, 0.75) : 0.0;\n"
    "  out_color = get_color(make_color(v, float(tex_type_of(id)) / 8.0, float(tex_sampler_of(id)), 1.0));\n"
    "}\n";

  std::string error;
  CHECK(shader.prepare_spirv(nullptr, shaderc_fragment_shader, &error));
  CHECK(error.empty());
  CHECK(shader.prepared(shaderc_fragment_shader));
  CHECK_FALSE(shader.spirv.empty());
}

TEST_CASE("shader_crafter keeps legacy bindings shared include alias [painter]") {
  painter::glsl_source_file shader;
  shader.memory =
    "#version 450\n"
    "#include <bindings/shared.h>\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "  out_color = vec4(prng_normalize(prng(1u)), 0.0, 0.0, 1.0);\n"
    "}\n";

  std::string error;
  CHECK(shader.prepare_spirv(nullptr, shaderc_fragment_shader, &error));
  CHECK(error.empty());
  CHECK(shader.prepared(shaderc_fragment_shader));
  CHECK_FALSE(shader.spirv.empty());
}

TEST_CASE("painter render config reads demiurg tavl list subresources [painter]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_painter_list_config_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "render_config" / "resources");
  fs::create_directories(root / "core" / "render_config" / "render_targets");
  fs::create_directories(root / "core" / "render_config" / "render_graphs");
  fs::create_directories(root / "core" / "render_config" / "materials");

  {
    std::ofstream out(root / "core" / "render_config" / "declare_values.tavl");
    out << "{\n";
    out << "  name = viewport\n";
    out << "  type = screensize\n";
    out << "}\n";
  }

  {
    std::ofstream out(root / "core" / "render_config" / "declare_counters.tavl");
    out << "per_frame\n";
    out << "per_update\n";
    out << "swapchain\n";
  }

  {
    std::ofstream out(root / "core" / "render_config" / "resources" / "list.tavl");
    out << "{\n";
    out << "  name = swapchain_image\n";
    out << "  format = swapchain4\n";
    out << "  role = present\n";
    out << "  size = viewport\n";
    out << "  type = swapchain\n";
    out << "  swap = swapchain\n";
    out << "}\n";
    out << "//---\n";
    out << "{\n";
    out << "  name = albedo_res\n";
    out << "  format = c4\n";
    out << "  role = gbuffer_albedo\n";
    out << "  size = viewport\n";
    out << "  type = frames_in_flight\n";
    out << "  swap = per_frame\n";
    out << "}\n";
  }

  {
    std::ofstream out(root / "core" / "render_config" / "render_targets" / "list.tavl");
    out << "{\n";
    out << "  name = rt1\n";
    out << "  resources = [ albedo_res = color_attachment ]\n";
    out << "}\n";
  }

  {
    std::ofstream out(root / "core" / "render_config" / "materials" / "defined.tavl");
    out << "{\n";
    out << "  name = defined_material\n";
    out << "  definitions = [ PF01_CHECKER_WALL = \"1\" ]\n";
    out << "  shaders = {\n";
    out << "    vertex = \"defined.vert.glsl\"\n";
    out << "    fragment = \"defined.frag.glsl\"\n";
    out << "  }\n";
    out << "  depth = {\n";
    out << "    test = false\n";
    out << "    write = false\n";
    out << "    compare = less_or_equal\n";
    out << "  }\n";
    out << "  raster = {\n";
    out << "    cull = none\n";
    out << "    front_face = cw\n";
    out << "    polygon = fill\n";
    out << "    line_width = 1.0\n";
    out << "  }\n";
    out << "}\n";
  }

  {
    std::ofstream out(root / "core" / "render_config" / "render_graphs" / "main.tavl");
    out << "{\n";
    out << "  name = graphics1\n";
    out << "  startup = true\n";
    out << "  passes = [ { name = p1 render_target = rt1 subpasses = [ { albedo_res = (color_attachment, store) } ] } ]\n";
    out << "  present_source = swapchain_image\n";
    out << "}\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});

  demiurg::resource_system resources;
  resources.register_type<painter::render_config_source>("render_config", "tavl");
  resources.parse_resources(&modules);

  auto* swapchain = resources.get<painter::render_config_source>("render_config/resources/list:swapchain_image");
  auto* albedo = resources.get<painter::render_config_source>("render_config/resources/list:albedo_res");
  REQUIRE(swapchain != nullptr);
  REQUIRE(albedo != nullptr);
  CHECK(resources.get("render_config/resources/list:0") == swapchain);
  CHECK(resources.get("render_config/resources/list:1") == albedo);

  std::vector<painter::render_config_source*> sources;
  resources.find<painter::render_config_source>("render_config", sources);
  for (auto* src : sources) {
    src->load(utils::safe_handle_t{});
  }

  const auto storage = painter::build_render_config(&resources, "render_config/");
  CHECK(storage.find_resource("swapchain_image") != painter::invalid_resource_slot);
  CHECK(storage.find_resource("albedo_res") != painter::invalid_resource_slot);
  CHECK(storage.find_render_target("rt1") != painter::invalid_resource_slot);
  const auto material_slot = storage.find_material("defined_material");
  REQUIRE(material_slot != painter::invalid_resource_slot);
  REQUIRE(storage.materials[material_slot].definitions.size() == 1);
  CHECK(storage.materials[material_slot].definitions[0].first == "PF01_CHECKER_WALL");
  CHECK(storage.materials[material_slot].definitions[0].second == "1");
  CHECK(storage.find_render_graph("graphics1") != painter::invalid_resource_slot);
  REQUIRE(storage.graphs.size() == 1);
  CHECK(storage.graphs.front().startup);
  CHECK(swapchain->state() == demiurg::state::warm);
  CHECK(albedo->state() == demiurg::state::warm);
  CHECK(swapchain->text.empty());
  CHECK(albedo->text.empty());
  CHECK(swapchain->list_section.empty());
  CHECK(albedo->list_section.empty());

  const auto storage2 = painter::build_render_config(&resources, "render_config/");
  CHECK(storage2.find_resource("swapchain_image") != painter::invalid_resource_slot);
  CHECK(storage2.find_resource("albedo_res") != painter::invalid_resource_slot);
  CHECK(swapchain->state() == demiurg::state::warm);
  CHECK(albedo->state() == demiurg::state::warm);
  CHECK(swapchain->text.empty());
  CHECK(albedo->text.empty());

  fs::remove_all(root);
}

TEST_CASE("specialization reflection reports id, type and size of shader constants [painter]") {
  painter::glsl_source_file shader;
  shader.memory =
    "#version 450\n"
    "layout(constant_id = 0) const uint pcf_radius = 1;\n"
    "layout(constant_id = 3) const float bias_scale = 0.5;\n"
    "layout(constant_id = 5) const bool use_contact = false;\n"
    "layout(constant_id = 9) const int signed_knob = -2;\n"
    "layout(location = 0) out vec4 out_color;\n"
    "void main() {\n"
    "  const float v = float(pcf_radius) * bias_scale * float(signed_knob);\n"
    "  out_color = vec4(use_contact ? v : 0.0, 0.0, 0.0, 1.0);\n"
    "}\n";

  std::string error;
  REQUIRE(shader.prepare_spirv(nullptr, shaderc_fragment_shader, &error));
  const auto reflected = painter::reflect_specialization_constants(shader.spirv, "test");

  const auto find_by_id = [&](const uint32_t id) -> const painter::specialization_constant* {
    const auto itr = std::find_if(reflected.begin(), reflected.end(), [&](const auto& entry) {
      return entry.constant_id == id;
    });
    return itr == reflected.end() ? nullptr : &(*itr);
  };

  REQUIRE(reflected.size() == 4);
  const auto* radius = find_by_id(0);
  const auto* bias = find_by_id(3);
  const auto* contact = find_by_id(5);
  const auto* knob = find_by_id(9);
  REQUIRE(radius != nullptr);
  REQUIRE(bias != nullptr);
  REQUIRE(contact != nullptr);
  REQUIRE(knob != nullptr);

  CHECK(radius->kind == painter::specialization_constant::value_kind::unsigned_integer);
  CHECK(radius->size == 4);
  CHECK(bias->kind == painter::specialization_constant::value_kind::floating);
  CHECK(contact->kind == painter::specialization_constant::value_kind::boolean);
  CHECK(contact->size == 4);
  CHECK(knob->kind == painter::specialization_constant::value_kind::signed_integer);

  // Оптимизированный модуль имён не содержит: spirv-opt снимает OpName. Поэтому constant_id/тип
  // всегда доступны, а имена приезжают отдельной сборкой с debug info.
  CHECK(radius->name.empty());

  painter::shader_crafter named(nullptr);
  named.set_optimization(false);
  named.set_debug_info(true);
  named.set_shader_entry_point("main");
  named.set_shader_type(shaderc_fragment_shader);
  const auto named_spirv = named.compile("test", shader.memory);
  REQUIRE_FALSE(named_spirv.empty());

  auto merged = reflected;
  painter::merge_specialization_names(merged, painter::reflect_specialization_constants(named_spirv, "test"));
  const auto merged_name_of = [&](const uint32_t id) {
    const auto itr = std::find_if(merged.begin(), merged.end(), [&](const auto& entry) {
      return entry.constant_id == id;
    });
    REQUIRE(itr != merged.end());
    return itr->name;
  };
  CHECK(merged_name_of(0) == "pcf_radius");
  CHECK(merged_name_of(3) == "bias_scale");
  CHECK(merged_name_of(5) == "use_contact");
  CHECK(merged_name_of(9) == "signed_knob");
}

TEST_CASE("specialization blob parses values by reflected type and reports strangers [painter]") {
  std::vector<painter::specialization_constant> reflected{
    {"pcf_radius", 0, 4, painter::specialization_constant::value_kind::unsigned_integer},
    {"bias_scale", 3, 4, painter::specialization_constant::value_kind::floating},
    {"use_contact", 5, 4, painter::specialization_constant::value_kind::boolean},
    {"", 7, 4, painter::specialization_constant::value_kind::signed_integer},
  };

  const std::vector<std::pair<std::string, std::string>> requested{
    {"pcf_radius", "3"},
    {"bias_scale", "0.25"},
    {"use_contact", "true"},
    {"id_7", "-5"},
    {"absent_knob", "1"},
  };

  std::vector<bool> matched(requested.size(), false);
  const auto blob = painter::build_specialization_blob(reflected, requested, "step", &matched);

  REQUIRE(blob.entries.size() == 4);
  CHECK(blob.data.size() == 16);
  // Порядок записей канонизирован по constant_id независимо от порядка в конфиге.
  CHECK(blob.entries[0].constant_id == 0);
  CHECK(blob.entries[1].constant_id == 3);
  CHECK(blob.entries[2].constant_id == 5);
  CHECK(blob.entries[3].constant_id == 7);

  const auto read_at = [&](const uint32_t constant_id, auto sample) {
    const auto itr = std::find_if(blob.entries.begin(), blob.entries.end(), [&](const auto& entry) {
      return entry.constant_id == constant_id;
    });
    REQUIRE(itr != blob.entries.end());
    REQUIRE(itr->size == sizeof(sample));
    std::memcpy(&sample, blob.data.data() + itr->offset, sizeof(sample));
    return sample;
  };

  CHECK(read_at(0, uint32_t{}) == 3u);
  CHECK(read_at(3, float{}) == doctest::Approx(0.25f));
  CHECK(read_at(5, uint32_t{}) == 1u);
  CHECK(read_at(7, int32_t{}) == -5);

  // 'absent_knob' в этой стадии не найден: сама сборка молчит, а loud error остаётся за вызывающим,
  // который видит все стадии сразу.
  CHECK(matched[0]);
  CHECK(matched[3]);
  CHECK_FALSE(matched[4]);
}

TEST_CASE("render config parses step shader constants and comparison samplers [painter]") {
  const auto storage = painter::build_render_config(PAINTER_TEST_CONFIG_ROOT);

  const auto step_slot = storage.find_execution_step("draw_regions");
  REQUIRE(step_slot != painter::invalid_resource_slot);
  const auto& step = storage.steps[step_slot];
  REQUIRE(step.shader_constants.size() == 2);
  CHECK(step.shader_constants[0].first == "region_quality");
  CHECK(step.shader_constants[0].second == "2");
  CHECK(step.shader_constants[1].first == "id_7");
  CHECK(step.shader_constants[1].second == "0.5");

  const auto sampler_slot = storage.find_sampler("shadow_compare");
  REQUIRE(sampler_slot != painter::invalid_resource_slot);
  const auto& compare_sampler = storage.samplers[sampler_slot];
  CHECK(compare_sampler.compare_enable == 1);
  CHECK(compare_sampler.compare_op == painter::compare_op::from_string("greater_or_equal"));
  const auto linear_slot = storage.find_sampler("linear");
  REQUIRE(linear_slot != painter::invalid_resource_slot);
  CHECK(compare_sampler.mag_filter == storage.samplers[linear_slot].mag_filter);

  const auto plain_slot = storage.find_sampler("nearest");
  REQUIRE(plain_slot != painter::invalid_resource_slot);
  CHECK(storage.samplers[plain_slot].compare_enable == 0);
}
