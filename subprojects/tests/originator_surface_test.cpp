#include <doctest/doctest.h>
#include "devils_engine/originator/field_surface.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <memory>
#include <optional>
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
// Клетки Куна вдали от уточнения: замкнутость поверхности через клетки, переходные клетки и чанки.
TEST_CASE("kuhn plain cells keep a closed manifold next to refinement and across chunks") {
  auto w = words();
  auto f = field(w, {{4, 4, 4}, 2.1, false});
  // Коробка задевает сферу у x = 2: поверхность проходит уточнённые, переходные и клетки Куна.
  field_box detail;
  detail.low = {1, 3, 3};
  detail.high = {2.5, 4.5, 4.5};
  for (const bool refined : {true, false}) {
    std::map<std::pair<point, point>, size_t> edges;
    size_t triangles = 0;
    for (int z = 0; z < 2; ++z)
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
          field_surface_options o;
          o.cells = 4;
          o.kuhn_plain = true;
          o.origin = {double(x * 4), double(y * 4), double(z * 4)};
          auto mesh = make_field_surface(f, o, refined ? std::span(&detail, 1) : std::span<const field_box>{});
          REQUIRE(mesh.size() % 3 == 0);
          triangles += mesh.size() / 3;
          for (size_t i = 0; i < mesh.size(); i += 3)
            for (size_t j = 0; j < 3; ++j) {
              auto a = quantize(mesh[i + j].position), b = quantize(mesh[i + (j + 1) % 3].position);
              if (b < a) std::swap(a, b);
              ++edges[{a, b}];
            }
        }
    REQUIRE(triangles > 0);
    for (const auto& [edge, count] : edges)
      CHECK(count == 2);
  }
}
// Без коробок поверхность строится по неподвижной решётке напрямую; коробка, не задевающая ни одной
// клетки, ведёт по общему пути. Обе дороги обязаны дать одну сетку бит в бит.
TEST_CASE("box-free surfaces match the general path bit for bit") {
  auto w = words();
  auto f = field(w, {{4, 4, 4}, 2.1, false});
  field_box far_away;
  far_away.low = {100, 100, 100};
  far_away.high = {101, 101, 101};
  for (const bool kuhn : {false, true}) {
    field_surface_options o;
    o.cells = 8;
    o.kuhn_plain = kuhn;
    const auto direct = make_field_surface(f, o, {});
    const auto general = make_field_surface(f, o, std::span(&far_away, 1));
    REQUIRE(!direct.empty());
    REQUIRE(direct.size() == general.size());
    for (size_t i = 0; i < direct.size(); ++i) {
      CHECK(direct[i].position == general[i].position);
      CHECK(direct[i].normal == general[i].normal);
      CHECK(direct[i].tag == general[i].tag);
    }
  }
}
namespace {
// Шар с объявленным наклоном и счётчиком вычислений: пропуск блоков виден по числу выборок формы.
struct counted_field {
  shape_vocabulary words;
  std::shared_ptr<size_t> evaluations = std::make_shared<size_t>(0);
  std::optional<primitive_field> field;
  counted_field(const field_vector centre, const double radius, const double slope) {
    words.set_background([](field_point&, bool&) { return shape_value{3, 0}; }, 3);
    words.set_background_slope(0);
    auto counter = evaluations;
    words.add_kind<shape>("shape", [](const generator_result&) { return std::vector<std::pair<shape, field_box>>{}; },
                          [counter](const shape& s, field_point& p) {
                            ++*counter;
                            auto q = p.position();
                            double x = q[0] - s.centre[0], y = q[1] - s.centre[1], z = q[2] - s.centre[2];
                            return shape_value{std::sqrt(x * x + y * y + z * z) - s.radius, 7};
                          });
    field_box box = field_box::around(centre, centre, radius + 40);
    box.slope = slope;
    std::vector<std::pair<shape, field_box>> rows{{shape{centre, radius, false}, box}};
    field_term_rows term{"shape", shape_vocabulary::pack(std::move(rows))};
    field.emplace(words, std::span(&term, 1));
  }
};
} // namespace
// Блок, чьи углы одного знака и дальше от нуля, чем наклон на половину диагонали, поверхности не
// содержит: пропуск экономит выборки и не меняет сетку ни на бит — без коробок и рядом с уточнением.
TEST_CASE("skipped empty blocks keep the kuhn surface bit for bit") {
  field_box detail;
  detail.low = {2, 7, 7};
  detail.high = {3.5, 9, 9};
  for (const bool refined : {false, true}) {
    counted_field full({8, 8, 8}, 5.3, 1), skipping({8, 8, 8}, 5.3, 1);
    field_surface_options o;
    o.cells = 16;
    o.kuhn_plain = true;
    const auto boxes = refined ? std::span<const field_box>(&detail, 1) : std::span<const field_box>{};
    const auto reference = make_field_surface(*full.field, o, boxes);
    o.skip_block = 2;
    const auto skipped = make_field_surface(*skipping.field, o, boxes);
    REQUIRE(!reference.empty());
    REQUIRE(reference.size() == skipped.size());
    for (size_t i = 0; i < reference.size(); ++i) {
      CHECK(reference[i].position == skipped[i].position);
      CHECK(reference[i].normal == skipped[i].normal);
      CHECK(reference[i].tag == skipped[i].tag);
    }
    // Решётка 17^3 = 4913 точек; нормали (семь выборок на вершину) пропуск не трогает.
    CHECK(*skipping.evaluations + 1000 < *full.evaluations);
  }
}
// Без объявленного наклона блоки не пропускаются; заниженный наклон ловит проверка соседних углов.
TEST_CASE("an undeclared slope skips nothing and an understated one fails loudly") {
  counted_field full({8, 8, 8}, 5.3, 1), unknown({8, 8, 8}, 5.3, std::numeric_limits<double>::infinity()), wrong({8, 8, 8}, 5.3, 0.2);
  field_surface_options o;
  o.cells = 16;
  o.kuhn_plain = true;
  const auto reference = make_field_surface(*full.field, o, {});
  o.skip_block = 2;
  CHECK(make_field_surface(*unknown.field, o, {}).size() == reference.size());
  CHECK(*unknown.evaluations >= *full.evaluations);
  CHECK_THROWS(make_field_surface(*wrong.field, o, {}));
  o.skip_block = 3;
  CHECK_THROWS(make_field_surface(*full.field, o, {}));
  o.skip_block = 2;
  o.kuhn_plain = false;
  CHECK_THROWS(make_field_surface(*full.field, o, {}));
}
// Нормали из решётки: те же вершины, нормали почти как у выборки в вершине и одинаковые у общей
// вершины соседних чанков (разности берут углы за границей чанка).
TEST_CASE("lattice normals follow sampled normals and agree across chunks") {
  counted_field f({8, 8, 8}, 5.3, 1);
  std::map<point, field_vector> shared;
  size_t vertices = 0;
  for (int z = 0; z < 2; ++z)
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        field_surface_options o;
        o.cells = 8;
        o.kuhn_plain = true;
        o.normal_step = 1;
        o.origin = {double(x * 8), double(y * 8), double(z * 8)};
        const auto sampled = make_field_surface(*f.field, o, {});
        o.lattice_normals = true;
        o.skip_block = 2;
        const auto lattice = make_field_surface(*f.field, o, {});
        REQUIRE(sampled.size() == lattice.size());
        vertices += lattice.size();
        for (size_t i = 0; i < lattice.size(); ++i) {
          CHECK(lattice[i].position == sampled[i].position);
          CHECK(lattice[i].tag == sampled[i].tag);
          const auto& a = lattice[i].normal;
          const auto& b = sampled[i].normal;
          CHECK(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] == doctest::Approx(1));
          CHECK(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] > 0.995);
          const auto [it, added] = shared.emplace(quantize(lattice[i].position), a);
          if (!added) CHECK(it->second == a);
        }
      }
  REQUIRE(vertices > 0);
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
