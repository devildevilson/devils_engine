#include <doctest/doctest.h>

#include <stdexcept>

#include "devils_engine/originator/motif_geometry.h"

using namespace devils_engine::originator;

namespace {

size_t builder_calls = 0;

motif_geometry_result chamfer(const motif_geometry_input& input) {
  if (input.parameters.size() != 1 || input.parameters[0].name != "cut") {
    return {{}, "chamfer needs cut"};
  }
  const auto cut = input.parameters[0].value;
  const auto w = int32_t(input.width), h = int32_t(input.height);
  if (cut < 1 || cut >= w || cut >= h) { return {{}, "cut does not fit"}; }
  return {{{{0,0},{w-cut,0},{w,cut},{w,h},{0,h}}}, {}};
}

motif_geometry_result concave(const motif_geometry_input&) {
  return {{{{0,0},{8,0},{4,4},{8,8},{0,8}}}, {}};
}

motif_geometry_result disconnected(const motif_geometry_input&) {
  return {{{{0,0},{4,0},{4,4},{0,4}}, {{8,0},{12,0},{12,4},{8,4}}}, {}};
}

motif_geometry_result touching_point(const motif_geometry_input&) {
  return {{{{0,0},{4,0},{4,4},{0,4}}, {{4,4},{8,4},{8,8},{4,8}}}, {}};
}

motif_geometry_result overlapping(const motif_geometry_input&) {
  return {{{{0,0},{4,0},{4,4},{0,4}}, {{2,0},{6,0},{6,4},{2,4}}}, {}};
}

motif_geometry_result joined(const motif_geometry_input&) {
  return {{{{0,0},{4,0},{4,4},{0,4}}, {{4,0},{8,0},{8,4},{4,4}}}, {}};
}

motif_geometry_result invalid_vertices(const motif_geometry_input&) {
  return {{{{0,0},{4,0},{4,4},{0,4},{0,0},{4,0},{4,4},{0,4}}}, {}};
}

motif_geometry_result counting_chamfer(const motif_geometry_input& input) {
  ++builder_calls;
  return chamfer(input);
}

motif_geometry_result clockwise(const motif_geometry_input&) {
  return {{{{0,0},{0,4},{4,4},{4,0}}}, {}};
}

motif_geometry_result excessive_coordinate(const motif_geometry_input&) {
  return {{{{0,0},{1'048'577,0},{1'048'577,4},{0,4}}}, {}};
}

} // namespace

TEST_CASE("registered motif geometry produces exact local polygons without a view") {
  motif_geometry_registry registry;
  add_standard_motif_geometry(registry);
  const auto rectangle = registry.build("rectangle", {12,10,0,{}});
  REQUIRE(rectangle.valid());
  REQUIRE(rectangle.pieces.size() == 1);
  CHECK(rectangle.pieces[0] == std::vector<motif_path_point>{{0,0},{12,0},{12,10},{0,10}});
  const auto rounded = registry.build("rounded_east", {12,10,0,{}});
  REQUIRE(rounded.valid());
  CHECK(rounded.pieces[0] == std::vector<motif_path_point>{{0,0},{10,0},{12,4},{12,6},{10,10},{0,10}});
  CHECK(registry.build("diagonal", {12,10,0,{}}).valid());
  CHECK_FALSE(registry.build("rounded_east", {7,10,0,{}}).valid());
  CHECK_FALSE(registry.build("diagonal", {6,10,0,{}}).valid());
}

TEST_CASE("project geometry can be registered without modifying standard dispatch") {
  motif_geometry_registry registry;
  add_standard_motif_geometry(registry);
  registry.add("project_chamfer", chamfer);
  const std::vector<motif_geometry_parameter> parameters{{"cut",3}};
  const auto first = registry.build("project_chamfer", {12,10,74,parameters});
  const auto again = registry.build("project_chamfer", {12,10,74,parameters});
  REQUIRE(first.valid());
  CHECK(first.pieces == again.pieces);
  CHECK(first.pieces[0][1] == motif_path_point{9,0});
  CHECK_FALSE(registry.build("project_chamfer", {12,10,74,{}}).valid());
  CHECK_THROWS_AS(registry.add("project_chamfer", chamfer), std::invalid_argument);
  CHECK_THROWS_AS(registry.add("", chamfer), std::invalid_argument);
  CHECK_THROWS_AS(registry.add("null", nullptr), std::invalid_argument);
}

TEST_CASE("invalid inputs refuse before invoking motif builders") {
  motif_geometry_registry registry;
  registry.add("project_chamfer", counting_chamfer);
  builder_calls = 0;
  CHECK(registry.build("missing", {12,10,0,{}}).refusal.find("missing") != std::string::npos);
  CHECK_FALSE(registry.build("project_chamfer", {0,10,0,{}}).valid());
  CHECK_FALSE(registry.build("project_chamfer", {4097,10,0,{}}).valid());
  const std::vector<motif_geometry_parameter> repeated{{"cut",1},{"cut",2}};
  CHECK(registry.build("project_chamfer", {12,10,0,repeated}).refusal.find("duplicate") != std::string::npos);
  CHECK(builder_calls == 0);
  const std::vector<motif_geometry_parameter> parameters{{"cut",3}};
  CHECK(registry.build("project_chamfer", {12,10,0,parameters}).valid());
  CHECK(builder_calls == 1);
}

TEST_CASE("all motif builders share convexity overlap and connectivity validation") {
  motif_geometry_registry registry;
  registry.add("concave", concave);
  registry.add("disconnected", disconnected);
  registry.add("point", touching_point);
  registry.add("overlap", overlapping);
  registry.add("joined", joined);
  registry.add("repeated", invalid_vertices);
  registry.add("clockwise", clockwise);
  registry.add("excessive", excessive_coordinate);
  for (const auto* name : {"concave","disconnected","point","overlap","repeated","clockwise","excessive"}) {
    const auto result = registry.build(name, {8,8,0,{}});
    INFO(name, ": ", result.refusal);
    CHECK_FALSE(result.valid());
    CHECK(result.pieces.empty());
    CHECK(result.refusal.find(name) != std::string::npos);
  }
  CHECK(registry.build("joined", {8,8,0,{}}).valid());
}
