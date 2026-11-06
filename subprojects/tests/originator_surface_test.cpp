#include <doctest/doctest.h>
#include "devils_engine/originator/field_surface.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <future>
#include <set>
using namespace devils_engine::originator;
namespace {
using point = std::array<int64_t, 3>;
point quantize(const field_vector& p) {
  return {int64_t(std::llround(p[0] * 1e8)), int64_t(std::llround(p[1] * 1e8)), int64_t(std::llround(p[2] * 1e8))};
}
struct shape {
  field_vector centre;
  double radius;
  bool tube;
};
shape_vocabulary words() {
  shape_vocabulary w;
  w.set_background([](field_point&, bool&) {
    return shape_value{3, 0};
  },
                   3);
  w.add_kind<shape>("shape", [](const generator_result&) {
    return std::vector<std::pair<shape, field_box>>{};
  },
                    [](const shape& s, field_point& p) {
                      auto q = p.position();
                      double x = s.tube ? 0 : q[0] - s.centre[0], y = q[1] - s.centre[1], z = q[2] - s.centre[2];
                      return shape_value{std::sqrt(x * x + y * y + z * z) - s.radius, 7};
                    });
  return w;
}
primitive_field field(shape_vocabulary& w, shape s) {
  std::vector<std::pair<shape, field_box>> rows{{s, field_box::around({4, 4, 4}, {4, 4, 4}, 20)}};
  field_term_rows term{"shape", shape_vocabulary::pack(std::move(rows))};
  return primitive_field(w, std::span(&term, 1));
}
} // namespace
TEST_CASE("locally refined surfaces form a closed manifold across cells and chunks") {
  auto w = words();
  auto f = field(w, {{4, 4, 4}, 2.1, false});
  field_box detail;
  detail.low = {1, 1, 1};
  detail.high = {4.2, 4.5, 4.5};
  std::map<std::pair<point, point>, size_t> edges;
  for (int z = 0; z < 2; ++z)
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        field_surface_options o;
        o.cells = 4;
        o.origin = {double(x * 4), double(y * 4), double(z * 4)};
        auto mesh = make_field_surface(f, o, std::span(&detail, 1));
        REQUIRE(mesh.size() % 3 == 0);
        for (size_t i = 0; i < mesh.size(); i += 3)
          for (size_t j = 0; j < 3; ++j) {
            auto a = quantize(mesh[i + j].position), b = quantize(mesh[i + (j + 1) % 3].position);
            if (b < a) std::swap(a, b);
            ++edges[{a, b}];
            double length = 0;
            for (double n : mesh[i + j].normal) {
              CHECK(std::isfinite(n));
              length += n * n;
            }
            CHECK(length == doctest::Approx(1));
            CHECK(mesh[i + j].tag == 7);
          }
      }
  REQUIRE(!edges.empty());
  for (const auto& [edge, count] : edges)
    CHECK(count == 2);
}
TEST_CASE("submetre water survives a coarse grid and has matching chunk borders") {
  auto w = words();
  auto f = field(w, {{0, 2.5, 2.5}, 0.32, true});
  field_box detail;
  detail.low = {-1, 1.8, 1.8};
  detail.high = {9, 3.2, 3.2};
  std::set<point> borders[2];
  for (int x = 0; x < 2; ++x) {
    field_surface_options o;
    o.cells = 4;
    o.origin = {double(x * 4), 0, 0};
    auto mesh = make_field_surface(f, o, std::span(&detail, 1));
    REQUIRE(!mesh.empty());
    for (const auto& v : mesh) {
      CHECK(std::hypot(v.position[1] - 2.5, v.position[2] - 2.5) <= 0.320001);
      if (std::abs(v.position[0] - 4) < 1e-9) borders[x].insert(quantize(v.position));
    }
  }
  CHECK(!borders[0].empty());
  CHECK(borders[0] == borders[1]);
  field_surface_options bad;
  bad.vertex_capacity = 3;
  CHECK_THROWS(make_field_surface(f, bad, std::span(&detail, 1)));
}

TEST_CASE("refined surfaces are identical in concurrent independent calls") {
  auto w = words();
  auto f = field(w, {{2, 2, 2}, 1.3, false});
  field_box detail;
  detail.low = {0.5, 0.5, 0.5};
  detail.high = {2.2, 2.2, 2.2};
  field_surface_options o;
  o.cells = 4;
  auto reference = make_field_surface(f, o, std::span(&detail, 1));
  std::vector<std::future<std::vector<field_surface_vertex>>> jobs;
  for (int i = 0; i < 4; ++i)
    jobs.push_back(std::async(std::launch::async, [&] {
      return make_field_surface(f, o, std::span(&detail, 1));
    }));
  for (auto& job : jobs) {
    auto result = job.get();
    REQUIRE(result.size() == reference.size());
    for (size_t i = 0; i < result.size(); ++i) {
      CHECK(result[i].position == reference[i].position);
      CHECK(result[i].normal == reference[i].normal);
      CHECK(result[i].tag == reference[i].tag);
    }
  }
  o.point_capacity = 1;
  CHECK_THROWS(make_field_surface(f, o, std::span(&detail, 1)));
  o.point_capacity = 2097152;
  o.tetrahedron_capacity = 1;
  CHECK_THROWS(make_field_surface(f, o, std::span(&detail, 1)));
}

TEST_CASE("normal scale changes shading while keeping the isosurface and fine normals") {
  auto w = words();
  auto f = field(w, {{2, 2, 2}, 1.3, false});
  field_box detail;
  detail.low = {0, 0, 0};
  detail.high = {2, 4, 4};
  field_surface_options o;
  o.cells = 4;
  auto reference = make_field_surface(f, o, std::span(&detail, 1));
  o.normal_step = 1;
  o.detail_normal_step = 0.05;
  auto smoothed = make_field_surface(f, o, std::span(&detail, 1));
  REQUIRE(reference.size() == smoothed.size());
  size_t fine = 0, changed = 0;
  for (size_t i = 0; i < reference.size(); ++i) {
    CHECK(reference[i].position == smoothed[i].position);
    CHECK(reference[i].tag == smoothed[i].tag);
    if (reference[i].position[0] <= 2) {
      CHECK(reference[i].normal == smoothed[i].normal);
      ++fine;
    } else
      for (size_t a = 0; a < 3; ++a)
        if (std::abs(reference[i].normal[a] - smoothed[i].normal[a]) > 1e-6) {
          ++changed;
          break;
        }
  }
  CHECK(fine > 0);
  CHECK(changed > 0);
}
