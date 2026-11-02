#include <cmath>
#include <memory>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/generator_chain.h"
#include "devils_engine/originator/primitive_field.h"
#include "devils_engine/originator/script_host.h"
#include "devils_engine/originator/script_shape.h"

using namespace devils_engine;

// Поле из примитивов на шарах: нативный генератор `balls` (ключ — номер набора) отдаёт центры и
// радиусы, lua-генератор `probe` объявляет поле из них и спрашивает его инструментами.
namespace {

constexpr const char* ball_buffers = R"(
{ name = balls, size = ball_capacity, format = [ centre = d3, radius = d1 ] }
{ name = ball_count, size = one, format = [ count = ui1 ] }
)";

constexpr const char* probe_buffers = R"(
{ name = points, size = probe_capacity, format = [ at = v3, direction = v3, value = v1, hit = v1 ] }
)";

constexpr const char* probe_body = R"(
return function(step)
  local p = step.writes.points
  local at, dir = p:field("at"), p:field("direction")
  at:set(0, 0, 0); at:set(0, 0, 1); at:set(0, 0, 2)
  at:set(1, 10, 0); at:set(1, 0, 1); at:set(1, 0, 2)
  at:set(2, 50, 0); at:set(2, 0, 1); at:set(2, 0, 2)
  for i = 0, 2 do dir:set(i, 1, 0) end
  originator.field_sample{ inputs = { at }, outputs = { p:field("value") }, params = { field = "world", count = 3 } }
  originator.field_march{ inputs = { at, dir }, outputs = { p:field("hit") },
                          params = { field = "world", count = 3, from = 0, to = 40, step = 0.25, want = "rock" } }
end
)";

constexpr const char* chain_text = R"(
name = field_test
generators = [
  { name = balls, native = balls, key = 1, buffers = ball_buffers, outputs = [ balls, ball_count ] },
  { name = probe, entry = probe, key = 1, outputs = [ points ],
    links = [ { name = set, from = balls, key = same } ],
    fields = [ { name = world, terms = [ { kind = ball, link = set } ] } ] }
]
)";

struct ball {
  originator::field_vector centre{};
  double radius = 0.0;
};

std::unique_ptr<originator::shape_vocabulary> make_vocabulary() {
  auto words = std::make_unique<originator::shape_vocabulary>();
  // Фон — порода значения 3; формы ниже него только внутри своих коробок.
  words->set_background([](originator::field_point&, bool&) { return originator::shape_value{3.0, 0}; }, 3.0);
  words->add_kind<ball>(
    "ball",
    [](const originator::generator_result& instance) {
      std::vector<std::pair<ball, originator::field_box>> rows;
      const auto centre = instance.field("balls", "centre"), radius = instance.field("balls", "radius");
      const auto count = size_t(instance.field("ball_count", "count").get(0));
      for (size_t i = 0; i < count; ++i) {
        ball b{{centre.get(i, 0), centre.get(i, 1), centre.get(i, 2)}, radius.get(i)};
        // Вне коробки (радиус + 3) расстояние до шара минус радиус не меньше фона.
        rows.emplace_back(b, originator::field_box::around(b.centre, b.centre, b.radius + 3.0));
      }
      return rows;
    },
    [](const ball& b, originator::field_point& p) {
      const auto& q = p.position();
      const double d = std::sqrt((q[0] - b.centre[0]) * (q[0] - b.centre[0]) + (q[1] - b.centre[1]) * (q[1] - b.centre[1]) +
                                 (q[2] - b.centre[2]) * (q[2] - b.centre[2]));
      return originator::shape_value{d - b.radius, uint32_t(b.radius)};
    });
  return words;
}

struct test_setup {
  std::unique_ptr<originator::shape_vocabulary> words;
  std::unique_ptr<originator::generator_chain> chain;
};

test_setup make() {
  test_setup out;
  out.words = make_vocabulary();
  originator::chain_setup setup;
  setup.native_buffers.emplace_back("balls", originator::parse_buffers(ball_buffers, "balls"));
  originator::pipeline_description probe;
  probe.name = "probe";
  probe.buffers = originator::parse_buffers(probe_buffers, "probe");
  probe.steps.push_back({.name = "ask", .body = "probe/ask", .reads = {}, .writes = {"points"}});
  setup.pipelines.emplace_back("probe", std::move(probe));
  setup.scripts.push_back({"probe", {{false, "ask", "probe/ask", probe_body}}});
  setup.sizes.set("one", 1);
  setup.sizes.set("ball_capacity", 8);
  setup.sizes.set("probe_capacity", 3);
  setup.vocabulary = out.words.get();
  out.chain = std::make_unique<originator::generator_chain>(originator::parse_chain(chain_text, "test"), std::move(setup));
  // Два шара на оси x: радиус 4 у начала и радиус 2 в x = 20.
  out.chain->set_native("balls", [](originator::generator_run& run) {
    auto& balls = run.output("balls");
    balls.field(0).set(0, 0.0, 0);
    balls.field(1).set(0, 4.0);
    balls.field(0).set(1, 20.0, 0);
    balls.field(1).set(1, 2.0);
    run.output("ball_count").field(0).set(0, 2);
  });
  out.chain->set_invoker_source(originator::make_chain_invokers(*out.chain, [](originator::tool_registry& tools) {
    tools.add_field_tools();
  }));
  return out;
}

} // namespace

TEST_CASE("primitive field: minimum of shapes under the background, exact culling by influence boxes") {
  auto test = make();
  const auto balls = test.chain->get("balls", 1, {{0, 0, 0}});
  const std::vector<originator::field_term_source> terms{{"ball", {balls}}};
  const originator::primitive_field field(*test.words, terms);
  CHECK(field.shape_count() == 2);

  CHECK(field.value({0, 0, 0}).value == -4.0);
  CHECK(field.value({0, 0, 0}).tag == 4);
  CHECK(field.value({21, 0, 0}).value == -1.0);
  CHECK(field.value({21, 0, 0}).tag == 2);
  CHECK(field.value({100, 0, 0}).value == 3.0); // только фон
  CHECK(field.value({100, 0, 0}).tag == 0);

  // Подготовленная область и поле целиком совпадают бит в бит по всей решётке, в том числе снаружи.
  const auto region = field.prepare({-6, -6, -6}, {6, 6, 6});
  CHECK(region.shape_count() == 1);
  size_t points = 0;
  for (double x = -10; x <= 30; x += 0.7) {
    for (double y = -6; y <= 6; y += 1.3) {
      const originator::field_vector p{x, y, 0.4};
      CHECK(region.value(p).value == field.value(p).value);
      ++points;
    }
  }
  CHECK(points > 500);

  originator::primitive_field::sampler samples(field, 8.0);
  CHECK(field.march(samples, {0, 0, 0}, {1, 0, 0}, 0.0, 40.0, 0.25, true) == doctest::Approx(4.25));
  CHECK(field.march(samples, {10, 0, 0}, {1, 0, 0}, 0.0, 40.0, 0.25, false) == doctest::Approx(8.0));
  CHECK(field.march(samples, {50, 0, 0}, {1, 0, 0}, 0.0, 40.0, 0.25, false) == -1.0);
}

TEST_CASE("primitive field: declared on a chain generator and asked from lua") {
  auto test = make();
  const auto probe = test.chain->get("probe", 1, {{0, 0, 0}});
  const auto value = probe->field("points", "value"), hit = probe->field("points", "hit");
  CHECK(value.get(0) == -4.0);
  CHECK(value.get(1) == 3.0);
  CHECK(value.get(2) == 3.0);
  CHECK(hit.get(0) == doctest::Approx(4.25)); // из воды шара 4 наружу
  CHECK(hit.get(1) == 0.0);                   // уже в породе
  CHECK(hit.get(2) == 0.0);
  CHECK(test.chain->verify("probe", 1, {{0, 0, 0}}).empty());
}

TEST_CASE("primitive field: roles — natural minimum, fill with a smooth seam, cut over the fill") {
  originator::shape_vocabulary words;
  // Вода везде: значение -5.
  words.set_background([](originator::field_point&, bool&) { return originator::shape_value{-5.0, 0}; }, -5.0);
  const auto distance = [](const ball& b, originator::field_point& p) {
    const auto& q = p.position();
    return std::sqrt((q[0] - b.centre[0]) * (q[0] - b.centre[0]) + (q[1] - b.centre[1]) * (q[1] - b.centre[1]) +
                     (q[2] - b.centre[2]) * (q[2] - b.centre[2]));
  };
  const auto no_decode = [](const originator::generator_result&) { return std::vector<std::pair<ball, originator::field_box>>{}; };
  words.add_kind<ball>("rock", no_decode, [distance](const ball& b, originator::field_point& p) {
    return originator::shape_value{b.radius - distance(b, p), 7};
  });
  words.add_kind<ball>("room", no_decode, [distance](const ball& b, originator::field_point& p) {
    return originator::shape_value{distance(b, p) - b.radius, 9};
  });
  words.set_role("rock", originator::shape_role::fill);
  words.set_role("room", originator::shape_role::cut);
  const ball rock{{0, 0, 0}, 10.0}, room{{0, 0, 0}, 3.0};
  const auto rows = [](const ball& b, const double reach) {
    std::vector<std::pair<ball, originator::field_box>> one;
    one.emplace_back(b, originator::field_box::around(b.centre, b.centre, reach));
    return originator::shape_vocabulary::pack(std::move(one));
  };
  // Порядок членов не важен: стадии идут по роли.
  const originator::field_term_rows terms[] = {{"room", rows(room, 8.0)}, {"rock", rows(rock, 16.0)}};
  const originator::primitive_field field(words, terms);
  CHECK(field.value({0, 0, 0}).value == -3.0); // помещение внутри своей породы
  CHECK(field.value({0, 0, 0}).tag == 9);
  CHECK(field.value({6, 0, 0}).value == 3.0); // стена помещения из залитой породы
  CHECK(field.value({6, 0, 0}).tag == 9);
  CHECK(field.value({8, 0, 0}).value == 2.0);
  CHECK(field.value({8, 0, 0}).tag == 7);
  CHECK(field.value({30, 0, 0}).value == -5.0); // вне коробки заливки — природная вода
  // Пакетный путь считает то же.
  const std::vector<originator::field_vector> points{{0, 0, 0}, {6, 0, 0}, {8, 0, 0}, {14, 0, 0}, {30, 0, 0}};
  std::vector<double> values(points.size());
  std::vector<uint32_t> tags(points.size());
  field.sample_points(points, values, tags, 4.0);
  for (size_t i = 0; i < points.size(); ++i) CHECK(values[i] == field.value(points[i]).value);

  // Шов: со сглаживанием заливка у границы чуть выше максимума, вдали от шва — ровно максимум.
  originator::shape_vocabulary smooth;
  smooth.set_background([](originator::field_point&, bool&) { return originator::shape_value{-5.0, 0}; }, -5.0);
  smooth.add_kind<ball>("rock", no_decode, [distance](const ball& b, originator::field_point& p) {
    return originator::shape_value{b.radius - distance(b, p), 7};
  });
  smooth.set_role("rock", originator::shape_role::fill, 2.0);
  const originator::field_term_rows rock_only[] = {{"rock", rows(rock, 16.0)}};
  const originator::primitive_field blended(smooth, rock_only);
  CHECK(blended.value({15, 0, 0}).value == doctest::Approx(-4.5)); // значения равны: + blend / 4
  CHECK(blended.value({0, 0, 0}).value == 10.0);
  CHECK_THROWS(smooth.set_role("rock", originator::shape_role::fill, -1.0));

  // Вставка: порода внутри выреза (колонна в помещении) — вырез её не стирает, вне колонны — вырез.
  words.add_kind<ball>("pillar", no_decode, [distance](const ball& b, originator::field_point& p) {
    return originator::shape_value{b.radius - distance(b, p), 11};
  });
  words.set_role("pillar", originator::shape_role::insert);
  const ball pillar{{0, 0, 0}, 1.0};
  const originator::field_term_rows with_pillar[] = {
    {"pillar", rows(pillar, 4.0)}, {"room", rows(room, 8.0)}, {"rock", rows(rock, 16.0)}};
  const originator::primitive_field pillared(words, with_pillar);
  CHECK(pillared.value({0, 0, 0}).value == 1.0); // внутри колонны — порода
  CHECK(pillared.value({0, 0, 0}).tag == 11);
  CHECK(pillared.value({2, 0, 0}).value == -1.0); // в помещении рядом с колонной — вода выреза
  CHECK(pillared.value({2, 0, 0}).tag == 9);
  const std::vector<originator::field_vector> pillar_points{{0, 0, 0}, {0.5, 0, 0}, {2, 0, 0}, {6, 0, 0}};
  std::vector<double> pillar_values(pillar_points.size());
  std::vector<uint32_t> pillar_tags(pillar_points.size());
  pillared.sample_points(pillar_points, pillar_values, pillar_tags, 4.0);
  for (size_t i = 0; i < pillar_points.size(); ++i) CHECK(pillar_values[i] == pillared.value(pillar_points[i]).value);
}

TEST_CASE("primitive field refusals") {
  auto test = make();
  CHECK_THROWS(originator::current_field("world")); // вне сборки экземпляра

  // Поле над связью, которой нет.
  originator::chain_setup setup;
  setup.native_buffers.emplace_back("balls", originator::parse_buffers(ball_buffers, "balls"));
  originator::pipeline_description probe;
  probe.name = "probe";
  probe.buffers = originator::parse_buffers(probe_buffers, "probe");
  probe.steps.push_back({.name = "ask", .body = "probe/ask", .reads = {}, .writes = {"points"}});
  setup.pipelines.emplace_back("probe", probe);
  setup.sizes.set("one", 1);
  setup.vocabulary = test.words.get();
  CHECK_THROWS(originator::generator_chain(originator::parse_chain(R"(
generators = [
  { name = balls, native = balls, key = 1, buffers = b, outputs = [ balls ] },
  { name = probe, entry = probe, key = 1, outputs = [ points ], fields = [ { name = w, terms = [ { kind = ball, link = nope } ] } ] }
]
)", "bad"), setup));
  // Поле без словаря форм.
  setup.vocabulary = nullptr;
  CHECK_THROWS(originator::generator_chain(originator::parse_chain(chain_text, "no words"), setup));
}

TEST_CASE("script composite applies child max and min inside one shape and culls by child boxes") {
  constexpr const char* buffers = R"(
{ name = main, size = one, format = [ low = d3, high = d3, radius = d1,
                                    first = ui1, count = ui1, hole_first = ui1, hole_count = ui1 ] }
{ name = details, size = two, format = [ low = d3, high = d3, amount = d1 ] }
{ name = holes, size = one, format = [ low = d3, high = d3, amount = d1 ] }
)";
  auto description = originator::parse_chain(R"(
generators = [ { name = shape, native = shape, buffers = shape_buffers,
                 outputs = [ main, details, holes ] } ]
)", "composite");
  originator::shape_vocabulary words;
  words.set_background([](originator::field_point&, bool&) { return originator::shape_value{3.0, 0}; }, 3.0);
  originator::chain_shape shape;
  shape.name = "composite"; shape.buffer = "main";
  shape.box_a = "low"; shape.box_b = "high"; shape.box = "aabb"; shape.reach = "0";
  shape.tag = "7"; shape.path = "test/base"; shape.text = "{ x - radius }";
  shape.parts.push_back({"details", "first", "count", "max", "test/detail", "{ child_amount }", "low", "high"});
  shape.parts.push_back({"holes", "hole_first", "hole_count", "min", "test/hole", "{ child_amount }", "low", "high"});
  originator::add_script_kind(words, shape);
  originator::chain_setup setup;
  setup.native_buffers.emplace_back("shape", originator::parse_buffers(buffers, "shape"));
  setup.sizes.set("one", 1); setup.sizes.set("two", 2);
  setup.vocabulary = &words;
  originator::generator_chain chain(std::move(description), std::move(setup));
  chain.set_native("shape", [](originator::generator_run& run) {
    auto& main = run.output("main");
    for (size_t axis = 0; axis < 3; ++axis) {
      main.field(0).set(0, axis == 0 ? -20.0 : -1.0, axis);
      main.field(1).set(0, axis == 0 ? 20.0 : 1.0, axis);
    }
    main.field(2).set(0, 2.0); main.field(3).set(0, 0); main.field(4).set(0, 2);
    main.field(5).set(0, 0); main.field(6).set(0, 1);
    auto& details = run.output("details");
    for (size_t i = 0; i < 2; ++i) {
      details.field(0).set(i, i == 0 ? -1.0 : 10.0, 0);
      details.field(1).set(i, i == 0 ? 1.0 : 12.0, 0);
      for (size_t axis = 1; axis < 3; ++axis) {
        details.field(0).set(i, -1.0, axis); details.field(1).set(i, 1.0, axis);
      }
      details.field(2).set(i, i == 0 ? 1.0 : 5.0);
    }
    auto& holes = run.output("holes");
    holes.field(0).set(0, 0.0, 0); holes.field(1).set(0, 0.5, 0);
    for (size_t axis = 1; axis < 3; ++axis) {
      holes.field(0).set(0, -1.0, axis); holes.field(1).set(0, 1.0, axis);
    }
    holes.field(2).set(0, -3.0);
  });
  const auto instance = chain.get("shape", 9, {{0, 0, 0}});
  const std::vector<originator::field_term_source> terms{{"composite", {instance}}};
  const originator::primitive_field field(words, terms);
  CHECK(field.value({0.25, 0, 0}).value == -3.0);
  CHECK(field.value({0.75, 0, 0}).value == 1.0);
  CHECK(field.value({6, 0, 0}).value == 3.0);
  CHECK(field.value({11, 0, 0}).value == 3.0);
  CHECK(field.value({-5, 0, 0}).value == -7.0);
  const std::vector<originator::field_vector> points{{0.25, 0, 0}, {0.75, 0, 0}, {6, 0, 0}, {11, 0, 0}, {-5, 0, 0}};
  std::vector<double> values(points.size());
  std::vector<uint32_t> tags(points.size());
  field.sample_points(points, values, tags, 12.0);
  for (size_t i = 0; i < points.size(); ++i) {
    CHECK(values[i] == field.value(points[i]).value);
    CHECK(tags[i] == field.value(points[i]).tag);
  }
}
