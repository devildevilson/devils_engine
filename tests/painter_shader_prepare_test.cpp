#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <shaderc/shaderc.h>
#include <vector>

#include "devils_engine/painter/glsl_source_file.h"
#include "devils_engine/painter/render_config_source.h"
#include "devils_engine/painter/structures.h"
#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"

using namespace devils_engine;

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
}

TEST_CASE("painter render config reads demiurg tavl list subresources [painter]") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_painter_list_config_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "render_config" / "resources");
  fs::create_directories(root / "core" / "render_config" / "render_targets");
  fs::create_directories(root / "core" / "render_config" / "render_graphs");

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
    std::ofstream out(root / "core" / "render_config" / "render_graphs" / "main.tavl");
    out << "{\n";
    out << "  name = graphics1\n";
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
  for (auto* src : sources) src->load(utils::safe_handle_t{});

  const auto storage = painter::build_render_config(&resources, "render_config/");
  CHECK(storage.find_resource("swapchain_image") != painter::INVALID_RESOURCE_SLOT);
  CHECK(storage.find_resource("albedo_res") != painter::INVALID_RESOURCE_SLOT);
  CHECK(storage.find_render_target("rt1") != painter::INVALID_RESOURCE_SLOT);
  CHECK(storage.find_render_graph("graphics1") != painter::INVALID_RESOURCE_SLOT);

  fs::remove_all(root);
}
