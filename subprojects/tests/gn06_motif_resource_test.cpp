#include <doctest/doctest.h>

#include <fstream>
#include <iterator>
#include <stdexcept>

#include "../playgrounds/GN06_dungeon_layout/src/site_motif_resource.h"

namespace {

const std::string minimal = R"(
version = 1
profiles = [compact]
motifs = [ {
  name = laboratory
  geometry = { operation = rounded_east, width = [12], height = [10] }
  placement = { strategy = quarter_turn, entrance_edge = 5, wall_edges = [0,2,4,5] }
  presentation = { glyph = "L", colour = [125,116,95] }
} ]
)";

std::string changed(const std::string& source, const std::string_view from, const std::string_view to) {
  auto result = source;
  result.replace(result.find(from), from.size(), to);
  return result;
}

} // namespace

TEST_CASE("a site motif is independently readable geometry placement and presentation") {
  const auto catalogue = gn06::read_site_motifs(minimal);
  CHECK(catalogue.version == 1);
  CHECK(catalogue.profiles == std::vector<std::string>{"compact"});
  const auto& motif = gn06::find_site_motif(catalogue, "laboratory");
  CHECK(motif.geometry.operation == "rounded_east");
  CHECK(motif.geometry.width[0] == 12);
  CHECK(motif.placement.entrance_edge == 5);
  CHECK(motif.presentation.glyph == "L");
  CHECK_THROWS_WITH_AS(gn06::find_site_motif(catalogue, "missing"),
    "site motifs: unknown motif 'missing'", std::runtime_error);
}

TEST_CASE("motif resource refuses unknown procedures invalid dimensions and ports") {
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"rounded_east","mystery")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"width = [12]","width = [7]")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"width = [12]","width = [12,13]")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"entrance_edge = 5","entrance_edge = 6")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"quarter_turn","edge_parallel")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"[125,116,95]","[256,116,95]")), std::runtime_error);
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(minimal,"profiles = [compact]","profiles = [compact,compact]")), std::runtime_error);
}

TEST_CASE("the shipped site motifs need no legacy motif programme or theme") {
  std::ifstream stream(std::string(GN06_TEST_RESOURCE_ROOT) + "/generator/site_motifs.tavl");
  REQUIRE(stream.good());
  const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  const auto catalogue = gn06::read_site_motifs(source);
  REQUIRE(catalogue.motifs.size() == 12);
  const auto& gallery = gn06::find_site_motif(catalogue,"gallery");
  CHECK(gallery.geometry.operation == "path");
  CHECK(gallery.geometry.path.bend_max == std::vector<int32_t>{10,14,18});
  CHECK(gallery.geometry.path.end.accent == "service_alcove");
  CHECK(gn06::find_site_motif(catalogue,"watch_post").placement.strategy == "edge_parallel");
  CHECK(gn06::find_site_motif(catalogue,"observation_walk").placement.strategy == "parent_wrap");
  CHECK_THROWS_AS(gn06::read_site_motifs(changed(source,"l1_max = [650,650,650]",
    "l1_max = [400,650,650]")), std::runtime_error);
}
