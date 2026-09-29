#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/motif_layout.h"
#include "devils_engine/originator/motif_path.h"

// МОТИВ — АВТОРСКИЙ СТРОИТЕЛЬНЫЙ БЛОК, а не название формы в ядре. Проверки держат границу:
// программа оставляет выбор координат и размеров алгоритму, но результат обязан быть обходимым,
// повторяемым и честным о каждом пересечении с поверхностью.

using namespace devils_engine::originator;

namespace {

std::vector<motif_rule> sample_program() {
  return {
    {1, 10, no_motif_parent, 15, 19, 3, 4, 0, 100},
    {2, 10, 0, 5, 7, 5, 7, motif_east, 100},
    {3, 20, 1, 9, 12, 8, 11, motif_east, 100},
    {4, 10, 0, 4, 5, 4, 5, uint8_t(motif_north | motif_south), 100},
    {4, 10, 0, 4, 5, 4, 5, uint8_t(motif_north | motif_south), 100},
  };
}

} // namespace

TEST_CASE("authored motifs form a traversable multi-location plan") {
  const auto rules = sample_program();
  for (uint64_t seed = 0; seed < 48; ++seed) {
    const auto layout = assemble_motifs(rules, 64, 48, 24, seed);
    INFO("seed ", seed, ": ", layout.refusal);
    REQUIRE(layout.valid());
    CHECK(validate_motif_layout(layout).empty());
    REQUIRE(layout.instances.size() == 5);
    CHECK(layout.passages.size() == layout.instances.size());
    CHECK(std::count_if(layout.instances.begin(), layout.instances.end(),
                        [](const auto& item) { return item.location == 10; }) == 4);
    CHECK(std::count_if(layout.instances.begin(), layout.instances.end(),
                        [](const auto& item) { return item.location == 20; }) == 1);
    const auto again = assemble_motifs(rules, 64, 48, 24, seed);
    CHECK(again.instances == layout.instances);
    CHECK(again.passages == layout.passages);
  }
}

TEST_CASE("surface intersections are returned without changing the plan") {
  const auto rules = sample_program();
  const auto base = assemble_motifs(rules, 64, 48, 24, 71);
  REQUIRE(base.valid());
  const auto& rect = base.instances[2].rect;
  const auto x = rect.x + rect.w / 2;
  const auto y = rect.y + rect.h / 2;
  std::vector<uint8_t> surface(64 * 48, 0);
  surface[size_t(y) * 64 + x] = 1;
  const auto cut = assemble_motifs(rules, 64, 48, 24, 71, surface);
  REQUIRE(cut.valid());
  CHECK(cut.instances == base.instances);
  CHECK(cut.passages == base.passages);
  REQUIRE(cut.exposures.size() == 1);
  CHECK(cut.exposures[0] == motif_exposure{2, x, y});
}

TEST_CASE("edge alignment names the start or end of a parent's wall") {
  std::vector<motif_rule> rules{
    {1, 1, no_motif_parent, 30, 30, 4, 4, 0, 100},
    {2, 1, 0, 5, 5, 5, 5, motif_north, 100},
  };
  for (uint64_t seed = 0; seed < 16; ++seed) {
    rules[1].align = motif_attach_align::start;
    const auto start = assemble_motifs(rules, 80, 80, 40, seed);
    INFO("start seed ", seed, ": ", start.refusal);
    REQUIRE(start.valid());
    CHECK(start.instances[1].rect.x == start.instances[0].rect.x);

    rules[1].align = motif_attach_align::end;
    const auto end = assemble_motifs(rules, 80, 80, 40, seed);
    INFO("end seed ", seed, ": ", end.refusal);
    REQUIRE(end.valid());
    CHECK(end.instances[1].rect.x ==
          end.instances[0].rect.x + end.instances[0].rect.w - end.instances[1].rect.w);
  }
}

TEST_CASE("centre alignment keeps a centred child and passage on the same wall") {
  std::vector<motif_rule> rules{
    {1, 1, no_motif_parent, 20, 20, 12, 12, 0, 100},
    {2, 2, 0, 8, 8, 12, 12, motif_east, 100},
  };
  rules[1].align = motif_attach_align::centre;
  rules[1].passage_align = motif_attach_align::centre;
  rules[1].strict_align = true;
  for (uint64_t seed = 0; seed < 16; ++seed) {
    const auto layout = assemble_motifs(rules, 64, 48, 24, seed);
    INFO("centre seed ", seed, ": ", layout.refusal);
    REQUIRE(layout.valid());
    CHECK(layout.instances[1].rect.y == layout.instances[0].rect.y);
    CHECK(layout.passages[1].y == layout.instances[0].rect.y + 6);
  }
}

TEST_CASE("direct motif joins share an edge without inventing a passage cell") {
  std::vector<motif_rule> rules{
    {1, 1, no_motif_parent, 18, 18, 5, 5, 0, 100},
    {2, 1, 0, 8, 8, 8, 8, motif_east, 100},
    {3, 1, 1, 7, 7, 5, 5, motif_east, 100},
  };
  rules[1].align = motif_attach_align::start;
  rules[1].direct_join = true;
  rules[2].align = motif_attach_align::end;
  rules[2].direct_join = true;
  for (uint64_t seed = 0; seed < 16; ++seed) {
    auto layout = assemble_motifs(rules, 80, 48, 24, seed);
    INFO("direct seed ", seed, ": ", layout.refusal);
    REQUIRE(layout.valid());
    REQUIRE(layout.instances.size() == 3);
    CHECK(layout.passages.size() == 1); // только внешний вход
    CHECK(layout.instances[1].rect.x == layout.instances[0].rect.x + layout.instances[0].rect.w);
    CHECK(layout.instances[2].rect.x == layout.instances[1].rect.x + layout.instances[1].rect.w);
    CHECK(validate_motif_layout(layout).empty());
    layout.instances[2].rect.x += 1;
    CHECK(validate_motif_layout(layout).find("direct motif join") != std::string::npos);
  }
}

TEST_CASE("passage inset keeps an edge port away from the miter corner") {
  std::vector<motif_rule> rules{
    {1, 1, no_motif_parent, 20, 20, 12, 12, 0, 100},
    {2, 2, 0, 8, 8, 12, 12, motif_east, 100},
  };
  rules[1].align = motif_attach_align::start;
  rules[1].passage_align = motif_attach_align::start;
  rules[1].passage_inset = 2;
  const auto start = assemble_motifs(rules, 64, 48, 24, 8);
  REQUIRE(start.valid());
  CHECK(start.passages[1].y == start.instances[0].rect.y + 2);
  rules[1].passage_align = motif_attach_align::end;
  const auto end = assemble_motifs(rules, 64, 48, 24, 8);
  REQUIRE(end.valid());
  CHECK(end.passages[1].y == end.instances[0].rect.y + 9);
  rules[1].passage_inset = 100;
  const auto clamped = assemble_motifs(rules, 64, 48, 24, 8);
  REQUIRE(clamped.valid());
  CHECK(clamped.passages[1].y == clamped.instances[0].rect.y + 6);
}

TEST_CASE("one path width survives its offset bend and shared miter sections") {
  for (const auto bend : {-12, -6, 6, 12}) {
    const auto path = make_motif_path({48, 6, 500, 250, bend});
    INFO(path.refusal);
    REQUIRE(path.valid());
    REQUIRE(path.pieces.size() == 3);
    CHECK(path.pieces[0].outline[1] == path.pieces[1].outline[0]);
    CHECK(path.pieces[0].outline[2] == path.pieces[1].outline[3]);
    CHECK(path.pieces[1].outline[1] == path.pieces[2].outline[0]);
    CHECK(path.pieces[1].outline[2] == path.pieces[2].outline[3]);
    for (size_t i = 0; i < path.pieces.size(); ++i) {
      for (size_t j = i + 1; j < path.pieces.size(); ++j) {
        CHECK_FALSE(convex_interiors_overlap(path.pieces[i].outline, path.pieces[j].outline));
      }
      const auto a = path.pieces[i].outline[0], b = path.pieces[i].outline[1];
      const auto c = path.pieces[i].outline[3];
      const auto dx = double(b.x - a.x), dy = double(b.y - a.y);
      const auto width = std::abs(dx * (c.y - a.y) - dy * (c.x - a.x)) / std::sqrt(dx * dx + dy * dy);
      CHECK(std::abs(width - 6.0) <= 1.0);
    }
    CHECK(path.axis2.back().y - path.axis2.front().y == 2 * bend);
  }
}

TEST_CASE("path fractions collapse to a straight or an explicit perpendicular remainder") {
  const auto straight = make_motif_path({48, 6, 750, 250, 8});
  REQUIRE(straight.valid());
  CHECK(straight.pieces.size() == 1);
  CHECK(straight.axis2.back() == motif_path_point{96, 6});
  CHECK(make_motif_path({48, 6, 550, 250, 0}).pieces.size() == 1);
  for (const auto direction : {-1, 1}) {
    const auto corner = make_motif_path({48, 6, 500, 0, direction, motif_path_mode::corner});
    REQUIRE(corner.valid());
    REQUIRE(corner.pieces.size() == 2);
    CHECK(corner.axis2.back() == motif_path_point{48, 6 + 48 * direction});
    CHECK(corner.pieces[0].outline[1] == corner.pieces[1].outline[0]);
    CHECK(corner.pieces[0].outline[2] == corner.pieces[1].outline[3]);
    CHECK_FALSE(convex_interiors_overlap(corner.pieces[0].outline, corner.pieces[1].outline));
  }
  CHECK_FALSE(make_motif_path({48, 6, 50, 250, 8}).valid());
  const std::vector<motif_path_point> a{{0, 0}, {6, 0}, {6, 6}, {0, 6}};
  const std::vector<motif_path_point> touching{{6, 0}, {12, 0}, {12, 6}, {6, 6}};
  const std::vector<motif_path_point> overlap{{5, 0}, {12, 0}, {12, 6}, {5, 6}};
  CHECK_FALSE(convex_interiors_overlap(a, touching));
  CHECK(convex_interiors_overlap(a, overlap));
}

TEST_CASE("impossible programs refuse and a severed passage fails validation") {
  auto rules = sample_program();
  rules[1].parent = 3;
  CHECK(assemble_motifs(rules, 64, 48, 24, 5).refusal.find("earlier parent") != std::string::npos);
  rules = sample_program();
  rules[1].strict_align = true;
  CHECK(assemble_motifs(rules, 64, 48, 24, 5).refusal.find("strictly align") != std::string::npos);
  rules = sample_program();
  CHECK(assemble_motifs(rules, 8, 8, 4, 5).refusal.find("size range") != std::string::npos);
  CHECK(assemble_motifs(rules, 64, 48, 24, 5, std::vector<uint8_t>(7)).refusal.find("mask") != std::string::npos);

  const std::vector<motif_rule> crowded{
    {1, 1, no_motif_parent, 15, 15, 3, 3, 0, 100},
    {2, 1, 0, 10, 10, 10, 10, motif_east, 100},
  };
  const auto refused = assemble_motifs(crowded, 24, 24, 12, 5, {}, 2);
  CHECK(refused.refusal.find("rule 1") != std::string::npos);
  CHECK(refused.attempts == 2);

  auto layout = assemble_motifs(rules, 64, 48, 24, 5);
  REQUIRE(layout.valid());
  layout.passages[2].x = 63;
  CHECK_FALSE(validate_motif_layout(layout).empty());
}

TEST_CASE("path ports exclude internal sections and short LM obeys an authored limit") {
  const auto short_bend = make_motif_path({48, 6, 500, 499, 8, motif_path_mode::offset, 1});
  INFO(short_bend.refusal);
  REQUIRE(short_bend.valid());
  REQUIRE(short_bend.pieces.size() == 3);
  CHECK(short_bend.axis2[2].x - short_bend.axis2[1].x == 2);
  CHECK_FALSE(make_motif_path({48, 6, 500, 499, 8, motif_path_mode::offset, 2}).valid());
  REQUIRE(short_bend.ports.size() == 8);
  for (const auto& port : short_bend.ports) {
    if (port.kind == motif_path_port_kind::wall) CHECK((port.edge == 0 || port.edge == 2));
    if (port.kind == motif_path_port_kind::start) CHECK((port.piece == 0 && port.edge == 3));
    if (port.kind == motif_path_port_kind::end) CHECK((port.piece == 2 && port.edge == 1));
  }
  const auto smaller = make_motif_path({48, 6, 500, 250, 4});
  const auto larger = make_motif_path({48, 6, 500, 250, 12});
  REQUIRE(smaller.valid());
  REQUIRE(larger.valid());
  CHECK(smaller.axis2.back().y != larger.axis2.back().y);
  CHECK(smaller.pieces[1].outline != larger.pieces[1].outline);
}

TEST_CASE("path trimming protects the exact shared interval and treats both ends equally") {
  const std::vector<motif_path_point> outline{{0, 0}, {40, 0}, {40, 6}, {0, 6}};
  const std::vector<motif_path_point> neighbour{{12, 6}, {18, 6}, {18, 12}, {12, 12}};
  const auto shared = shared_motif_boundary(outline, neighbour);
  REQUIRE(shared.size() == 1);
  const std::vector<motif_path_point> pins{shared[0].a, shared[0].b};
  const auto start = trim_motif_path_end(outline, 3, pins, 2, 4);
  INFO(start.refusal);
  REQUIRE(start.valid());
  CHECK(start.removed == 10);
  CHECK(start.outline[0] == motif_path_point{10, 0});
  CHECK(start.outline[3] == motif_path_point{10, 6});
  const auto end = trim_motif_path_end(start.outline, 1, pins, 2, 4);
  REQUIRE(end.valid());
  CHECK(end.removed == 20);
  CHECK(end.outline[1] == motif_path_point{20, 0});
  CHECK(end.outline[2] == motif_path_point{20, 6});
  CHECK(share_motif_boundary(end.outline, neighbour));
  const auto retained = shared_motif_boundary(end.outline, neighbour);
  REQUIRE(retained.size() == 1);
  CHECK(retained[0].a == shared[0].a);
  CHECK(retained[0].b == shared[0].b);
}

TEST_CASE("path trimming leaves occupied caps and open corner attachments untouched") {
  const auto shape = make_motif_path({40, 6, 750, 250, 0});
  REQUIRE(shape.valid());
  const auto& outline = shape.pieces[0].outline;
  const std::vector<motif_path_point> cap_pins{{0, 1}, {0, 5}};
  const auto occupied = trim_motif_path_end(outline, 3, cap_pins, 2, 4);
  REQUIRE(occupied.valid());
  CHECK(occupied.removed == 0);
  CHECK(occupied.outline == outline);
  const std::vector<motif_path_point> corner_pins{{0, 0}, {6, 0}};
  const auto tail = trim_motif_path_end(outline, 1, corner_pins, 2, 4);
  REQUIRE(tail.valid());
  CHECK(tail.outline[1] == motif_path_point{8, 0});
  CHECK(tail.outline[2] == motif_path_point{8, 6});
}

TEST_CASE("path trimming keeps miters, convexity and width after quarter turns") {
  for (const auto mode : {motif_path_mode::offset, motif_path_mode::corner}) {
    const auto path = make_motif_path({48, 6, 500, 250, 6, mode});
    REQUIRE(path.valid());
    for (const auto start : {true, false}) {
      const auto piece = start ? 0u : uint32_t(path.pieces.size() - 1);
      const auto edge = start ? 3u : 1u;
      auto outline = path.pieces[piece].outline;
      for (uint32_t turn = 0; turn < 4; ++turn) {
        const auto trimmed = trim_motif_path_end(outline, edge, {}, 2, 4);
        INFO(trimmed.refusal);
        REQUIRE(trimmed.valid());
        REQUIRE(trimmed.removed > 0);
        CHECK(trimmed.outline[(edge + 2) % 4] == outline[(edge + 2) % 4]);
        CHECK(trimmed.outline[(edge + 3) % 4] == outline[(edge + 3) % 4]);
        const auto a = trimmed.outline[edge], b = trimmed.outline[(edge + 1) % 4];
        CHECK(std::abs(a.x - b.x) + std::abs(a.y - b.y) == 6);
        for (auto& point : outline) {
          point = {-point.y, point.x};
        }
      }
    }
  }
}

TEST_CASE("path trimming refuses unsupported geometry and invalid protected points") {
  const std::vector<motif_path_point> outline{{0, 0}, {40, 0}, {40, 6}, {0, 6}};
  CHECK_FALSE(trim_motif_path_end(outline, 4, {}, 2, 4).valid());
  CHECK_FALSE(trim_motif_path_end(outline, 1, {}, 2, 0).valid());
  CHECK_FALSE(trim_motif_path_end(outline, 1, {}, 2, 41).valid());
  CHECK_FALSE(trim_motif_path_end(outline, 1, {}, 1'048'577, 4).valid());
  const std::vector<motif_path_point> outside{{41, 0}};
  CHECK_FALSE(trim_motif_path_end(outline, 1, outside, 2, 4).valid());
  const std::vector<motif_path_point> slanted{{0, 0}, {20, 10}, {18, 16}, {-2, 6}};
  CHECK_FALSE(trim_motif_path_end(slanted, 1, {}, 2, 4).valid());
  const std::vector<motif_path_point> point_touch{{40, 6}, {44, 6}, {44, 10}, {40, 10}};
  CHECK(shared_motif_boundary(outline, point_touch).empty());
}

TEST_CASE("path trimming preserves subunit port coordinates rather than snapping to cells") {
  const std::vector<motif_path_point> outline{{0, 0}, {10240, 0}, {10240, 1536}, {0, 1536}};
  const std::vector<motif_path_point> pins{{4609, 0}, {4865, 0}};
  const auto end = trim_motif_path_end(outline, 1, pins, 512, 1024);
  REQUIRE(end.valid());
  CHECK(end.outline[1].x == 5377);
  CHECK(end.removed == 4863);
  const auto start = trim_motif_path_end(end.outline, 3, pins, 512, 1024);
  REQUIRE(start.valid());
  CHECK(start.outline[0].x == 4097);
  CHECK(start.removed == 4097);
}

TEST_CASE("oblique apertures meet both real walls for several widths") {
  for (const auto bend : {-14, -6, 6, 14}) {
    const auto path = make_motif_path({48, 8, 500, 250, bend});
    REQUIRE(path.valid());
    auto wall = path.pieces[1].outline;
    for (auto& point : wall) {
      point.x *= 256;
      point.y *= 256;
    }
    for (const auto width : {256u, 512u, 768u, 1024u}) {
      const auto port = make_motif_edge_port(wall, 0, width, 500, 256);
      INFO(port.refusal);
      REQUIRE(port.valid());
      const auto attached = attach_motif_rect(port, 10 * 256, 7 * 256, 256);
      INFO(attached.refusal);
      REQUIRE(attached.valid());
      CHECK(share_motif_boundary(wall, attached.passage));
      CHECK(share_motif_boundary(attached.body, attached.passage));
      CHECK_FALSE(convex_interiors_overlap(wall, attached.passage));
      CHECK_FALSE(convex_interiors_overlap(wall, attached.body));
      CHECK_FALSE(convex_interiors_overlap(attached.body, attached.passage));
      const auto open = attach_motif_rect(port, 10 * 256, 7 * 256, 0);
      REQUIRE(open.valid());
      CHECK(open.passage.empty());
      CHECK(share_motif_boundary(wall, open.body));
      auto severed = attached.passage;
      for (auto& point : severed) --point.y;
      CHECK_FALSE(share_motif_boundary(wall, severed));
    }
    CHECK_FALSE(make_motif_edge_port(wall, 0, 100 * 256).valid());
  }
}

TEST_CASE("a shared boundary is a segment rather than a point or raster proximity") {
  const std::vector<motif_path_point> a{{0,0},{100,0},{100,100},{0,100}};
  const std::vector<motif_path_point> touching{{100,20},{150,20},{150,80},{100,80}};
  const std::vector<motif_path_point> point{{100,100},{150,100},{150,150},{100,150}};
  const std::vector<motif_path_point> separated{{101,20},{150,20},{150,80},{101,80}};
  CHECK(share_motif_boundary(a, touching));
  CHECK_FALSE(share_motif_boundary(a, point));
  CHECK_FALSE(share_motif_boundary(a, separated));
  CHECK_FALSE(make_motif_edge_port(a, 9, 20).valid());
  CHECK_FALSE(make_motif_edge_port(a, 0, 20, 1001).valid());
}

TEST_CASE("a varied path profile can bypass an obstacle that blocks the straight") {
  const std::vector<motif_path_point> obstacle{{30,0},{34,0},{34,3},{30,3}};
  const auto straight = make_motif_path({48,6,750,250,12});
  const auto detour = make_motif_path({48,6,500,250,12});
  REQUIRE(straight.valid());
  REQUIRE(detour.valid());
  CHECK(convex_interiors_overlap(straight.pieces.front().outline, obstacle));
  for (const auto& part : detour.pieces) {
    CHECK_FALSE(convex_interiors_overlap(part.outline, obstacle));
  }
}
