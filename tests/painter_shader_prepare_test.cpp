#include <doctest/doctest.h>

#include <shaderc/shaderc.h>

#include "devils_engine/painter/glsl_source_file.h"

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

