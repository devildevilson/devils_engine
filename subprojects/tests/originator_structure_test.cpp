#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/tools.h"

using namespace devils_engine;

// ПОСТРОЙКИ: комнаты, связи и вырезание.
//
// Проверяется здесь не «получилось похоже на подземелье», а то, что инструменты ОБЕЩАЮТ, и обещаний
// этих ДВА РАЗНЫХ РОДА. `place_rooms` обещает локальное: комнаты объявленного размера, в границах, не
// ближе зазора. `link_rooms` обещает ГЛОБАЛЬНОЕ: остов соединяет все комнаты. Второе не проверяется
// осмотром соседей — его видно только на всей карте сразу, — и ровно поэтому набор существует
// отдельно от остальных.
//
// И ещё одно разделение, которое тест держит намеренно: связность ПЛАНА и связность РАСТРА
// проверяются по отдельности. Между ними лежит вырезание, и разорвать второе, не тронув первое,
// вполне возможно.

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

constexpr size_t side = 48;
constexpr size_t capacity = 24;

originator::tool_registry& registry() {
  static originator::tool_registry r;
  if (r.size() == 0) {
    r.add_structure_tools();
  }
  return r;
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

struct scene {
  originator::buffer rooms;
  originator::buffer links;
  originator::buffer cells;
  originator::buffer state;
};

scene make_scene(const size_t room_capacity = capacity, const size_t link_capacity = capacity) {
  scene result;

  const std::vector<field_pair> room_fields = {{"rect", "ui4"}};
  result.rooms = originator::buffer(
    "rooms", originator::make_buffer_layout(originator::storage_kind::soa, room_fields, "rooms"), room_capacity);

  const std::vector<field_pair> link_fields = {{"pair", "ui2"}, {"path", "ui4"}};
  result.links = originator::buffer(
    "links", originator::make_buffer_layout(originator::storage_kind::soa, link_fields, "links"), link_capacity);

  const std::vector<field_pair> cell_fields = {{"room", "ui1"}, {"corridor", "ub1"}};
  result.cells = originator::buffer(
    "cells", originator::make_buffer_layout(originator::storage_kind::soa, cell_fields, "cells"),
    originator::buffer_extent{side, side, 0});

  const std::vector<field_pair> state_fields = {{"rooms", "ui1"}, {"links", "ui1"}, {"attempts", "ui1"}};
  result.state = originator::buffer(
    "state", originator::make_buffer_layout(originator::storage_kind::soa, state_fields, "state"), size_t(1));
  return result;
}

struct rect {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;

  bool operator==(const rect& other) const noexcept = default;
};

void place(scene& target, const uint64_t seed, const int64_t gap = 2, const int64_t min_size = 4,
           const int64_t max_size = 8) {
  const auto* tool = registry().find("place_rooms");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("width", double(side));
  params.set_number("height", double(side));
  params.set_number("min_size", double(min_size));
  params.set_number("max_size", double(max_size));
  params.set_number("gap", double(gap));
  params.set_number("border", 1.0);

  const std::vector<originator::field_ref> outputs{writable(target.rooms, "rect"), writable(target.state, "rooms"),
                                                   writable(target.state, "attempts")};
  originator::dispatch(*tool, {}, outputs, params, seed, 0, target.rooms.count(), "plan", nullptr);
}

size_t room_count(const scene& target) {
  return size_t(target.state.field(target.state.find_field("rooms")).get(0));
}

size_t link_count(const scene& target) {
  return size_t(target.state.field(target.state.find_field("links")).get(0));
}

std::vector<rect> read_rooms(const scene& target) {
  const auto field = target.rooms.field(target.rooms.find_field("rect"));
  std::vector<rect> result(room_count(target));
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = rect{int64_t(field.get(i, 0)), int64_t(field.get(i, 1)), int64_t(field.get(i, 2)),
                     int64_t(field.get(i, 3))};
  }
  return result;
}

void link(scene& target, const uint64_t seed, const int64_t extra = 0) {
  const auto* tool = registry().find("link_rooms");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("extra_links", double(extra));

  const std::vector<originator::field_ref> inputs{readable(target.rooms, "rect"), readable(target.state, "rooms")};
  const std::vector<originator::field_ref> outputs{writable(target.links, "pair"), writable(target.state, "links")};
  originator::dispatch(*tool, inputs, outputs, params, seed, 0, target.links.count(), "links", nullptr);
}

std::vector<std::pair<uint32_t, uint32_t>> read_links(const scene& target) {
  const auto field = target.links.field(target.links.find_field("pair"));
  std::vector<std::pair<uint32_t, uint32_t>> result(link_count(target));
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = {uint32_t(field.get(i, 0)), uint32_t(field.get(i, 1))};
  }
  return result;
}

// Концы коридоров ВЫВОДЯТСЯ из плана отдельным проходом: копия центров в списке связей однажды
// разъехалась бы с комнатами, а `carve_corridors` про комнаты знать не обязан — он режет колено
// между двумя точками.
void trace(scene& target) {
  const auto* tool = registry().find("link_path");
  REQUIRE(tool != nullptr);

  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.links, "pair"), readable(target.rooms, "rect")};
  const std::vector<originator::field_ref> outputs{writable(target.links, "path")};
  originator::dispatch(*tool, inputs, outputs, empty, 1, 0, link_count(target), "trace", nullptr);
}

void carve(scene& target, const uint64_t seed, const int64_t corridor_width = 1) {
  const auto* rooms_tool = registry().find("paint_rects");
  const auto* corridors_tool = registry().find("carve_corridors");
  REQUIRE(rooms_tool != nullptr);
  REQUIRE(corridors_tool != nullptr);

  trace(target);

  const originator::parameters empty;
  const std::vector<originator::field_ref> room_inputs{readable(target.rooms, "rect")};
  const std::vector<originator::field_ref> room_outputs{writable(target.cells, "room")};
  originator::dispatch(*rooms_tool, room_inputs, room_outputs, empty, seed, 0, room_count(target), "carve", nullptr);

  originator::parameters params;
  params.set_number("corridor_width", double(corridor_width));
  const std::vector<originator::field_ref> link_inputs{readable(target.links, "path")};
  const std::vector<originator::field_ref> link_outputs{writable(target.cells, "corridor")};
  originator::dispatch(*corridors_tool, link_inputs, link_outputs, params, seed, 0, link_count(target), "carve",
                       nullptr);
}

// Сколько кусков у ПЛАНА: связи как рёбра, комнаты как вершины.
size_t plan_components(const size_t rooms, const std::vector<std::pair<uint32_t, uint32_t>>& links) {
  std::vector<uint32_t> parent(rooms);
  for (size_t i = 0; i < rooms; ++i) {
    parent[i] = uint32_t(i);
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };
  for (const auto& edge : links) {
    parent[root(edge.first)] = root(edge.second);
  }
  size_t components = 0;
  for (size_t i = 0; i < rooms; ++i) {
    components += size_t(root(uint32_t(i)) == i);
  }
  return components;
}

// Сколько клеток пола НЕ достижимо заливкой от первой найденной. Это и есть обещание постройки, и
// живёт оно на растре, а не в плане.
size_t unreachable_floor(const scene& target) {
  const auto rooms = target.cells.field(target.cells.find_field("room"));
  const auto corridors = target.cells.field(target.cells.find_field("corridor"));
  const size_t count = target.cells.count();

  const auto floor = [&](const size_t index) {
    return rooms.get(index) != 0.0 || corridors.get(index) != 0.0;
  };

  size_t total = 0;
  size_t start = count;
  for (size_t i = 0; i < count; ++i) {
    if (!floor(i)) continue;
    ++total;
    if (start == count) start = i;
  }
  if (start == count) {
    return 0;
  }

  std::vector<uint8_t> seen(count, 0);
  std::vector<size_t> stack{start};
  seen[start] = 1;
  size_t reached = 0;
  while (!stack.empty()) {
    const auto index = stack.back();
    stack.pop_back();
    ++reached;

    const int64_t x = int64_t(index % side);
    const int64_t y = int64_t(index / side);
    const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& step : steps) {
      const int64_t nx = x + step[0];
      const int64_t ny = y + step[1];
      if (nx < 0 || ny < 0 || nx >= int64_t(side) || ny >= int64_t(side)) continue;
      const auto next = size_t(ny) * side + size_t(nx);
      if (seen[next] != 0 || !floor(next)) continue;
      seen[next] = 1;
      stack.push_back(next);
    }
  }
  return total - reached;
}
} // namespace

TEST_CASE("originator place_rooms keeps every room inside the bounds and apart from the others") {
  auto target = make_scene();
  place(target, 20260912);
  const auto rooms = read_rooms(target);

  REQUIRE(rooms.size() > 4);
  CHECK(rooms.size() <= capacity);

  for (const auto& room : rooms) {
    CHECK(room.w >= 4);
    CHECK(room.h >= 4);
    CHECK(room.w <= 8);
    CHECK(room.h <= 8);
    CHECK(room.x >= 1);
    CHECK(room.y >= 1);
    CHECK(room.x + room.w <= int64_t(side) - 1);
    CHECK(room.y + room.h <= int64_t(side) - 1);
  }

  // ЗАЗОР — отдельное обещание: комнаты, касающиеся стенами, на растре сливаются в одну комнату
  // странной формы, и план начинает описывать не то, что вырезано.
  size_t touching = 0;
  for (size_t a = 0; a < rooms.size(); ++a) {
    for (size_t b = a + 1; b < rooms.size(); ++b) {
      touching += size_t(rooms[a].x < rooms[b].x + rooms[b].w + 2 && rooms[b].x < rooms[a].x + rooms[a].w + 2 &&
                         rooms[a].y < rooms[b].y + rooms[b].h + 2 && rooms[b].y < rooms[a].y + rooms[a].h + 2);
    }
  }
  CHECK(touching == 0);
}

TEST_CASE("originator place_rooms says how many rooms it placed, and leaves the rest of the list empty") {
  auto target = make_scene();
  place(target, 4242);

  // ЁМКОСТЬ — ЖЕЛАНИЕ, СЧЁТЧИК — ФАКТ. Инструмент, прочитавший ёмкость вместо счётчика, получил бы
  // комнату нулевого размера в углу — и вырезал бы её.
  const auto field = target.rooms.field(target.rooms.find_field("rect"));
  for (size_t i = room_count(target); i < capacity; ++i) {
    CHECK(field.get(i, 2) == 0.0);
    CHECK(field.get(i, 3) == 0.0);
  }
  CHECK(target.state.field(target.state.find_field("attempts")).get(0) > 0.0);
}

TEST_CASE("originator place_rooms repeats itself under the same seed and moves under another") {
  auto first = make_scene();
  auto again = make_scene();
  auto other = make_scene();
  place(first, 777);
  place(again, 777);
  place(other, 778);

  // Ни одно решение здесь не принимается сравнением плавающих чисел: размеры и координаты целые, а
  // выбор — остаток от деления хеша.
  CHECK(read_rooms(first) == read_rooms(again));
  CHECK(read_rooms(first) != read_rooms(other));
}

TEST_CASE("originator place_rooms refuses bounds that cannot hold a single room") {
  auto target = make_scene();
  const auto* tool = registry().find("place_rooms");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("width", 6.0);
  params.set_number("height", 6.0);
  params.set_number("min_size", 8.0);
  params.set_number("border", 1.0);

  const std::vector<originator::field_ref> outputs{writable(target.rooms, "rect"), writable(target.state, "rooms")};
  CHECK_THROWS_AS(
    originator::dispatch(*tool, {}, outputs, params, 1, 0, target.rooms.count(), "plan", nullptr),
    std::exception);
}

TEST_CASE("originator link_rooms connects every room, and a tree needs no more than that") {
  auto target = make_scene();
  place(target, 20260912);
  link(target, 20260912);

  const auto rooms = room_count(target);
  const auto links = read_links(target);

  // ОСТОВ: соединяет ВСЕ комнаты и делает это ровно `комнаты - 1` связями. Оба числа — определение
  // дерева, а не свойство раскладки.
  CHECK(links.size() == rooms - 1);
  CHECK(plan_components(rooms, links) == 1);
}

TEST_CASE("originator link_rooms adds loops on top of a tree that is already complete") {
  // Ёмкость связей с запасом: остов занимает `комнаты - 1`, и петли добираются поверх.
  auto target = make_scene(capacity, capacity + 8);
  place(target, 20260912);
  link(target, 20260912, 3);

  const auto rooms = room_count(target);
  const auto links = read_links(target);
  CHECK(links.size() == rooms - 1 + 3);
  CHECK(plan_components(rooms, links) == 1);

  // Петля — это ВТОРОЙ путь, а не повтор первого: пара, уже соединённая остовом, петлёй не станет.
  size_t duplicates = 0;
  for (size_t a = 0; a < links.size(); ++a) {
    for (size_t b = a + 1; b < links.size(); ++b) {
      duplicates += size_t((links[a].first == links[b].first && links[a].second == links[b].second) ||
                           (links[a].first == links[b].second && links[a].second == links[b].first));
    }
  }
  CHECK(duplicates == 0);
}

TEST_CASE("originator link_rooms refuses a link list too short for a spanning tree") {
  auto target = make_scene(capacity, 3);
  place(target, 20260912);
  REQUIRE(room_count(target) > 4);

  // ГРОМКИЙ ОТКАЗ, А НЕ ОБРЕЗАНИЕ: недостроенный остов означает комнаты, до которых нельзя дойти, и
  // по карте этого не видно — она выглядит как обычная карта.
  CHECK_THROWS_AS(link(target, 20260912), std::exception);
}

TEST_CASE("originator link_path puts the corridor ends in the centres of the rooms it links") {
  auto target = make_scene(capacity, capacity + 8);
  place(target, 20260912);
  link(target, 20260912, 2);
  trace(target);

  const auto rooms = read_rooms(target);
  const auto links = read_links(target);
  const auto path = target.links.field(target.links.find_field("path"));
  for (size_t i = 0; i < links.size(); ++i) {
    const auto& a = rooms[links[i].first];
    const auto& b = rooms[links[i].second];
    // Центр считается ЦЕЛОЧИСЛЕННЫМ делением — тем же, которым его считал остов. Второй способ
    // посчитать ту же точку означал бы, что минимизировали расстояние не между теми точками,
    // которые потом соединяют.
    CHECK(path.get(i, 0) == double(a.x + a.w / 2));
    CHECK(path.get(i, 1) == double(a.y + a.h / 2));
    CHECK(path.get(i, 2) == double(b.x + b.w / 2));
    CHECK(path.get(i, 3) == double(b.y + b.h / 2));
  }
}

TEST_CASE("originator paint_rects marks every cell of a room with its number") {
  auto target = make_scene(capacity, capacity + 8);
  place(target, 20260912);
  link(target, 20260912);
  carve(target, 20260912);

  const auto rooms = read_rooms(target);
  const auto field = target.cells.field(target.cells.find_field("room"));
  size_t area = 0;
  for (size_t i = 0; i < rooms.size(); ++i) {
    const auto& room = rooms[i];
    area += size_t(room.w * room.h);
    for (int64_t y = room.y; y < room.y + room.h; ++y) {
      for (int64_t x = room.x; x < room.x + room.w; ++x) {
        REQUIRE(field.get(size_t(y) * side + size_t(x)) == double(i + 1));
      }
    }
  }

  size_t marked = 0;
  for (size_t i = 0; i < target.cells.count(); ++i) {
    marked += size_t(field.get(i) != 0.0);
  }
  CHECK(marked == area);
}

TEST_CASE("originator paint_rects refuses a room the raster cannot hold") {
  auto target = make_scene();
  auto rect_field = target.rooms.field(target.rooms.find_field("rect"));
  rect_field.set(0, double(side - 2), 0);
  rect_field.set(0, 2.0, 1);
  rect_field.set(0, 6.0, 2);
  rect_field.set(0, 6.0, 3);
  target.state.field(target.state.find_field("rooms")).set(0, 1.0);

  // Тихо обрезанная комната выглядит просто маленькой, и план с растром расходятся молча.
  const auto* tool = registry().find("paint_rects");
  REQUIRE(tool != nullptr);
  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.rooms, "rect")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "room")};
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, empty, 1, 0, 1, "carve", nullptr), std::exception);
}

TEST_CASE("originator carving leaves a dungeon where every floor cell is reachable") {
  for (const uint64_t seed : {uint64_t(1), uint64_t(20260912), uint64_t(99999)}) {
    auto target = make_scene(capacity, capacity + 8);
    place(target, seed);
    link(target, seed, 2);
    carve(target, seed);

    // ГЛАВНОЕ ОБЕЩАНИЕ, И ОНО ПРОВЕРЯЕТСЯ НА РАСТРЕ. Остов говорит, что связей достаточно; заливка
    // говорит, что вырезанное ими проходимо. Между этими утверждениями лежит вырезание.
    CHECK(unreachable_floor(target) == 0);
  }
}

TEST_CASE("originator carve_corridors keeps the dungeon whole when corridors get thick") {
  auto target = make_scene(capacity, capacity + 8);
  place(target, 20260912);
  link(target, 20260912, 2);
  carve(target, 20260912, 3);

  CHECK(unreachable_floor(target) == 0);
}

// ============================== ЗОНАЛЬНАЯ МОДЕЛЬ ==============================
//
// Здесь проверяется вторая постановка набора: не «комнаты и коридоры», а ЗОНЫ и их ВИДЫ. Обещания у
// неё те же по духу и разные по месту жительства: план обещает, что все места связаны СПИСКОМ
// СВЯЗЕЙ, геометрия — что вырезанное этими связями проходимо, а правила видов обещают, что
// запрещённой двери не появится ни ради связности, ни ради щедрости.
//
// Проверять их надо ПО ОТДЕЛЬНОСТИ и по разным представлениям — прямой урок PF09, где связь без
// общего ребра (лестница) и общее ребро без связи (перекрытие) существуют обе.

namespace {
constexpr size_t house_side = 32;
constexpr size_t zone_capacity = 96;
constexpr size_t join_capacity = 512;
// 0 — стена, 1 — коридор, 2 — место, 3 — улица. Улица объявлена ВСЕГДА, хотя пользуются ей не все
// проверки: обе матрицы считаются от числа видов, и дописывать вид по ходу значило бы менять их
// размер задним числом.
constexpr size_t kind_count = 4;
constexpr uint32_t wall_kind = 0;
constexpr uint32_t corridor_kind = 1;
constexpr uint32_t room_kind = 2;
constexpr uint32_t street_kind = 3;

struct zone_view {
  int64_t x = 0;
  int64_t y = 0;
  int64_t w = 0;
  int64_t h = 0;
  uint32_t kind = 0;

  bool operator==(const zone_view& other) const noexcept = default;
};

struct house_scene {
  originator::buffer zones;
  originator::buffer shapes;
  originator::buffer columns;
  originator::buffer joins;
  originator::buffer offsets;
  originator::buffer arcs;
  originator::buffer links;
  originator::buffer kinds;
  originator::buffer rules;
  originator::buffer cells;
  originator::buffer state;
};

house_scene make_house() {
  house_scene result;
  const std::vector<field_pair> zone_fields = {{"rect", "ui4"}, {"kind", "ui1"}};
  result.zones = originator::buffer(
    "zones", originator::make_buffer_layout(originator::storage_kind::soa, zone_fields, "zones"), zone_capacity);

  // ФОРМЫ И КОЛОННЫ живут рядом с зонами, а не внутри растра: после переезда плана в меру растр
  // перестал что-либо хранить, и всё, что раньше читалось с картинки, обязано лежать списком.
  const std::vector<field_pair> shape_fields = {{"shape", "ui2"}};
  result.shapes = originator::buffer(
    "shapes", originator::make_buffer_layout(originator::storage_kind::soa, shape_fields, "shapes"), zone_capacity);

  const std::vector<field_pair> column_fields = {{"rect", "ui4"}};
  result.columns = originator::buffer(
    "columns", originator::make_buffer_layout(originator::storage_kind::soa, column_fields, "columns"),
    zone_capacity);

  const std::vector<field_pair> join_fields = {{"pair", "ui3"}, {"span", "ui4"}};
  result.joins = originator::buffer(
    "joins", originator::make_buffer_layout(originator::storage_kind::soa, join_fields, "joins"), join_capacity);

  const std::vector<field_pair> offset_fields = {{"start", "ui1"}};
  result.offsets = originator::buffer(
    "zone_offsets", originator::make_buffer_layout(originator::storage_kind::soa, offset_fields, "zone_offsets"),
    zone_capacity + 1);

  const std::vector<field_pair> arc_fields = {{"zone", "ui1"}};
  result.arcs = originator::buffer(
    "zone_arcs", originator::make_buffer_layout(originator::storage_kind::soa, arc_fields, "zone_arcs"),
    join_capacity * 2);

  const std::vector<field_pair> link_fields = {{"edge", "ui4"}};
  result.links = originator::buffer(
    "links", originator::make_buffer_layout(originator::storage_kind::soa, link_fields, "links"), join_capacity);

  const std::vector<field_pair> kind_fields = {{"passable", "ub1"}, {"max_links", "ui1"}};
  result.kinds = originator::buffer(
    "kinds", originator::make_buffer_layout(originator::storage_kind::soa, kind_fields, "kinds"), kind_count);

  const std::vector<field_pair> rule_fields = {{"allowed", "ub1"}};
  result.rules = originator::buffer(
    "door_rules", originator::make_buffer_layout(originator::storage_kind::soa, rule_fields, "door_rules"),
    kind_count * kind_count);

  const std::vector<field_pair> cell_fields = {{"zone", "ui1"}};
  result.cells = originator::buffer(
    "cells", originator::make_buffer_layout(originator::storage_kind::soa, cell_fields, "cells"),
    originator::buffer_extent{house_side, house_side, 0});

  const std::vector<field_pair> state_fields = {{"zones", "ui1"}, {"joins", "ui1"}, {"places", "ui1"},
                                                {"links", "ui1"}, {"columns", "ui1"}, {"lost", "ui1"},
                                                {"merged", "ui1"}};
  result.state = originator::buffer(
    "state", originator::make_buffer_layout(originator::storage_kind::soa, state_fields, "state"), size_t(1));

  // Проходимость и пределы связей: у коридора предела нет, у места их два.
  auto passable = result.kinds.field(result.kinds.find_field("passable"));
  auto max_links = result.kinds.field(result.kinds.find_field("max_links"));
  passable.set(wall_kind, 0.0);
  passable.set(corridor_kind, 1.0);
  passable.set(room_kind, 1.0);
  passable.set(street_kind, 1.0);
  max_links.set(wall_kind, 0.0);
  max_links.set(corridor_kind, 0.0);
  max_links.set(room_kind, 2.0);
  max_links.set(street_kind, 0.0); // у улицы предела нет: дом на ней не один

  // Двери: место с коридором и коридор с коридором. Место с местом — НЕТ, и это тот самый жёсткий
  // запрет, который отличает зональную модель от «соединить что попало».
  auto allowed = result.rules.field(result.rules.find_field("allowed"));
  const auto permit = [&](const uint32_t a, const uint32_t b) {
    allowed.set(a * kind_count + b, 1.0);
    allowed.set(b * kind_count + a, 1.0);
  };
  permit(corridor_kind, corridor_kind);
  permit(corridor_kind, room_kind);
  return result;
}

void slice(house_scene& target, const uint64_t seed, const int64_t corridor = 2) {
  const auto* tool = registry().find("slice_zones");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("width", double(house_side));
  params.set_number("height", double(house_side));
  params.set_number("wall", 1.0);
  params.set_number("corridor", double(corridor));
  params.set_number("min_band", 4.0);
  params.set_number("max_band", 7.0);
  params.set_number("min_room", 3.0);
  params.set_number("max_room", 7.0);
  params.set_number("jitter", 1.0);
  params.set_number("corridor_kind", double(corridor_kind));
  params.set_number("room_kind", double(room_kind));

  const std::vector<originator::field_ref> outputs{writable(target.zones, "rect"), writable(target.zones, "kind"),
                                                   writable(target.state, "zones")};
  originator::dispatch(*tool, {}, outputs, params, seed, 0, zone_capacity, "layout", nullptr);
}

size_t zone_count(const house_scene& target) {
  return size_t(target.state.field(target.state.find_field("zones")).get(0));
}

size_t place_count(const house_scene& target) {
  return size_t(target.state.field(target.state.find_field("places")).get(0));
}

size_t house_link_count(const house_scene& target) {
  return size_t(target.state.field(target.state.find_field("links")).get(0));
}

void paint(house_scene& target, const size_t from, const size_t to) {
  const auto* tool = registry().find("paint_rects");
  REQUIRE(tool != nullptr);
  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "zone")};
  originator::dispatch(*tool, inputs, outputs, empty, 1, from, to, "paint", nullptr);
}

// СТЫКИ ВЫВОДЯТСЯ ИЗ ПЛАНА, а не из растра: клетки инструменту не передаются вовсе, и передать их
// больше некуда.
void find_joins(house_scene& target, const int64_t min_span = 1) {
  const auto* tool = registry().find("zone_joins");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("wall", 1.0);
  params.set_number("min_span", double(min_span));
  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(target.state, "zones"),
                                                  readable(target.shapes, "shape")};
  const std::vector<originator::field_ref> outputs{writable(target.joins, "pair"), writable(target.joins, "span"),
                                                   writable(target.state, "joins")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, join_capacity, "joins", nullptr);
}

void build_graph(house_scene& target) {
  const auto* tool = registry().find("zone_graph");
  REQUIRE(tool != nullptr);
  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.joins, "pair"), readable(target.state, "joins"),
                                                  readable(target.state, "zones")};
  const std::vector<originator::field_ref> outputs{writable(target.offsets, "start"), writable(target.arcs, "zone")};
  originator::dispatch(*tool, inputs, outputs, empty, 1, 0, zone_capacity + 1, "graph", nullptr);
}

void open_doors(house_scene& target, const int64_t extra = 0) {
  const auto* tool = registry().find("open_doors");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("wall", 1.0);
  params.set_number("door_width", 1.0);
  params.set_number("extra_doors", double(extra));
  params.set_number("door_kind", double(corridor_kind)); // дверь проходима и предела связей не несёт

  const std::vector<originator::field_ref> inputs{
    readable(target.joins, "pair"),  readable(target.joins, "span"),  readable(target.state, "joins"),
    readable(target.zones, "kind"),  readable(target.state, "zones"), readable(target.rules, "allowed"),
    readable(target.kinds, "max_links")};
  const std::vector<originator::field_ref> outputs{
    writable(target.zones, "rect"),   writable(target.zones, "kind"), writable(target.links, "edge"),
    writable(target.state, "places"), writable(target.state, "links")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, zone_capacity, "doors", nullptr);
}

// ПРОЕКЦИЯ: последний шаг и единственный, где появляются клетки. По умолчанию окно равно участку и
// масштаб один к одному — то есть картинка равна плану, и именно такую смотрят проверки растра.
void project(house_scene& target, const size_t places, const int64_t view_x = 0, const int64_t view_y = 0,
             const int64_t view_w = int64_t(house_side), const int64_t view_h = int64_t(house_side)) {
  const auto* tool = registry().find("project_zones");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("view_x", double(view_x));
  params.set_number("view_y", double(view_y));
  params.set_number("view_w", double(view_w));
  params.set_number("view_h", double(view_h));
  target.state.field(target.state.find_field("places")).set(0, double(places));
  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(target.state, "places"),
                                                  readable(target.shapes, "shape"),
                                                  readable(target.columns, "rect"),
                                                  readable(target.state, "columns")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "zone"), writable(target.state, "lost"),
                                                   writable(target.state, "merged")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, target.cells.count(), "draw", nullptr);
}

size_t lost_places(const house_scene& target) {
  return size_t(target.state.field(target.state.find_field("lost")).get(0));
}

size_t merged_borders(const house_scene& target) {
  return size_t(target.state.field(target.state.find_field("merged")).get(0));
}

std::vector<zone_view> read_zones(const house_scene& target, const size_t count) {
  const auto rect = target.zones.field(target.zones.find_field("rect"));
  const auto kind = target.zones.field(target.zones.find_field("kind"));
  std::vector<zone_view> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = zone_view{int64_t(rect.get(i, 0)), int64_t(rect.get(i, 1)), int64_t(rect.get(i, 2)),
                          int64_t(rect.get(i, 3)), uint32_t(kind.get(i))};
  }
  return result;
}

// Связность ПО СВЯЗЯМ: то самое обещание плана.
size_t house_components(const house_scene& target) {
  const auto edge = target.links.field(target.links.find_field("edge"));
  const auto places = place_count(target);
  std::vector<uint32_t> parent(places);
  for (size_t i = 0; i < places; ++i) {
    parent[i] = uint32_t(i);
  }
  const auto root = [&](uint32_t node) {
    while (parent[node] != node) {
      parent[node] = parent[parent[node]];
      node = parent[node];
    }
    return node;
  };
  for (size_t i = 0; i < house_link_count(target); ++i) {
    parent[root(uint32_t(edge.get(i, 0)))] = root(uint32_t(edge.get(i, 1)));
  }
  size_t components = 0;
  for (size_t i = 0; i < zone_count(target); ++i) {
    components += size_t(root(uint32_t(i)) == i);
  }
  return components;
}
} // namespace

TEST_CASE("originator slice_zones divides the footprint without overlaps and keeps the bands declared") {
  auto target = make_house();
  slice(target, 20260912);
  const auto zones = read_zones(target, zone_count(target));

  REQUIRE(zones.size() > 8);

  int64_t area = 0;
  size_t corridors = 0;
  for (const auto& zone : zones) {
    CHECK(zone.x >= 1);
    CHECK(zone.y >= 1);
    CHECK(zone.x + zone.w <= int64_t(house_side) - 1);
    CHECK(zone.y + zone.h <= int64_t(house_side) - 1);
    area += zone.w * zone.h;
    corridors += size_t(zone.kind == corridor_kind);
  }
  CHECK(corridors > 0);

  // ПЕРЕКРЫТИЕ НА РАСТРЕ НЕВИДИМО: вторая зона просто затирает первую. Поэтому считается площадь
  // против помеченных клеток — единственное, что ловит перекрытие.
  project(target, zones.size());
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  size_t painted = 0;
  for (size_t i = 0; i < target.cells.count(); ++i) {
    painted += size_t(cells.get(i) != 0.0);
  }
  CHECK(painted == size_t(area));
}

TEST_CASE("originator slice_zones repeats itself under the same seed and moves under another") {
  auto first = make_house();
  auto again = make_house();
  auto other = make_house();
  slice(first, 4242);
  slice(again, 4242);
  slice(other, 4243);

  CHECK(read_zones(first, zone_count(first)) == read_zones(again, zone_count(again)));
  CHECK(read_zones(other, zone_count(other)) != read_zones(first, zone_count(first)));
}

TEST_CASE("originator slice_zones refuses a footprint its declared plan cannot fill") {
  auto target = make_house();
  const auto* tool = registry().find("slice_zones");
  REQUIRE(tool != nullptr);

  originator::parameters params;
  params.set_number("width", double(house_side));
  params.set_number("height", 8.0);
  params.set_number("wall", 1.0);
  params.set_number("corridor", 2.0);
  params.set_number("min_band", 9.0); // две полосы по девять в восемь клеток не влезут
  params.set_number("max_band", 12.0);

  const std::vector<originator::field_ref> outputs{writable(target.zones, "rect"), writable(target.zones, "kind"),
                                                   writable(target.state, "zones")};
  CHECK_THROWS_AS(originator::dispatch(*tool, {}, outputs, params, 1, 0, zone_capacity, "layout", nullptr),
                  std::exception);
}

TEST_CASE("originator zone_joins tells a wall between two places from a plain touch") {
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);

  const auto pair = target.joins.field(target.joins.find_field("pair"));
  const auto span = target.joins.field(target.joins.find_field("span"));
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  const auto count = size_t(target.state.field(target.state.find_field("joins")).get(0));
  REQUIRE(count > 0);

  size_t direct = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto a = size_t(pair.get(i, 0));
    const auto b = size_t(pair.get(i, 1));
    const bool through = pair.get(i, 2) != 0.0;
    const int64_t x = int64_t(span.get(i, 0));
    const int64_t y = int64_t(span.get(i, 1));
    const int64_t length = int64_t(span.get(i, 2));
    const int64_t axis = int64_t(span.get(i, 3));

    REQUIRE(a != b);
    REQUIRE(a < zone_count(target));
    REQUIRE(b < zone_count(target));
    CHECK(length > 0);

    direct += size_t(!through);
    if (!through) {
      continue;
    }
    // Стык ЧЕРЕЗ СТЕНУ обязан быть именно им: по всей длине пробега между зонами лежит ничья клетка,
    // а по обе стороны — те самые две зоны. Иначе дверь оказалась бы не в стене.
    for (int64_t step = 0; step < length; ++step) {
      const int64_t cx = axis == 0 ? x : x + step;
      const int64_t cy = axis == 0 ? y + step : y;
      CHECK(cells.get(size_t(cy) * house_side + size_t(cx)) == 0.0);
      const auto first = cells.get(size_t(axis == 0 ? cy : cy - 1) * house_side + size_t(axis == 0 ? cx - 1 : cx));
      const auto second = cells.get(size_t(axis == 0 ? cy : cy + 1) * house_side + size_t(axis == 0 ? cx + 1 : cx));
      CHECK(((first == double(a + 1) && second == double(b + 1)) ||
             (first == double(b + 1) && second == double(a + 1))));
    }
  }

  // Прямые стыки обязаны быть: коридорные ленты примыкают к хребту без стены, и это связь, которой
  // дверь не нужна.
  CHECK(direct > 0);
}

TEST_CASE("originator zone_graph builds a symmetric adjacency without repeats") {
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);
  build_graph(target);

  const auto offsets = target.offsets.field(target.offsets.find_field("start"));
  const auto arcs = target.arcs.field(target.arcs.find_field("zone"));
  const auto zones = zone_count(target);

  for (size_t zone = 0; zone < zones; ++zone) {
    const auto first = size_t(offsets.get(zone));
    const auto last = size_t(offsets.get(zone + 1));
    CHECK(first <= last);
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      CHECK(other != zone);
      if (k + 1 < last) {
        // Упорядочены и без повторов: два стыка одной пары говорят решателю то же, что один.
        CHECK(arcs.get(k) < arcs.get(k + 1));
      }
      bool back = false;
      for (size_t j = size_t(offsets.get(other)); j < size_t(offsets.get(other + 1)); ++j) {
        back = back || size_t(arcs.get(j)) == zone;
      }
      CHECK(back);
    }
  }
}

TEST_CASE("originator open_doors connects every place and never breaks a declared prohibition") {
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);
  open_doors(target, 4);

  const auto places = place_count(target);
  const auto planned = zone_count(target);
  CHECK(places > planned); // двери дописались в тот же список: дверь это место

  // 1. ОБЕЩАНИЕ: все места связаны.
  CHECK(house_components(target) == 1);

  // 2. ЗАПРЕТ СОБЛЮДЁН: связи «место с местом» нет ни одной, хотя общих стен у мест сколько угодно.
  const auto edge = target.links.field(target.links.find_field("edge"));
  const auto kind = target.zones.field(target.zones.find_field("kind"));
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  size_t forbidden = 0;
  size_t doors = 0;
  size_t spare = 0;
  for (size_t i = 0; i < house_link_count(target); ++i) {
    const auto a = size_t(edge.get(i, 0));
    const auto b = size_t(edge.get(i, 1));
    forbidden += size_t(kind.get(a) == double(room_kind) && kind.get(b) == double(room_kind));
    doors += size_t(edge.get(i, 2) != 0.0);
    spare += size_t(edge.get(i, 3) != 0.0);
  }
  CHECK(forbidden == 0);
  CHECK(doors > 0);
  CHECK(spare == 4);

  // 3. ДВЕРЬ ЛЕЖИТ В СТЕНЕ: её клетки были ничьими до открытия, а теперь несут её номер. Картинка
  //    для этого рисуется ЗАНОВО — дверь появилась в плане, а не в растре, и увидеть её можно только
  //    спроецировав план ещё раз.
  project(target, places);
  const auto rect = target.zones.field(target.zones.find_field("rect"));
  for (size_t zone = planned; zone < places; ++zone) {
    const auto x = size_t(rect.get(zone, 0));
    const auto y = size_t(rect.get(zone, 1));
    CHECK(cells.get(y * house_side + x) == double(zone + 1));
  }
}

TEST_CASE("originator open_doors refuses when the declared rules leave a place unreachable") {
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);

  // Запрещаем дверь между местом и коридором. Связать места больше нечем — «место с местом» тоже
  // запрещено, — и это НЕ повод открыть запрещённую дверь: это повод отказать вслух.
  auto allowed = target.rules.field(target.rules.find_field("allowed"));
  allowed.set(corridor_kind * kind_count + room_kind, 0.0);
  allowed.set(room_kind * kind_count + corridor_kind, 0.0);

  CHECK_THROWS_AS(open_doors(target), std::exception);
}

TEST_CASE("originator open_doors honours the link limit declared by the kind") {
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);

  // Одна связь на место: предел ЖЁСТКИЙ, и щедрость его не обходит.
  target.kinds.field(target.kinds.find_field("max_links")).set(room_kind, 1.0);
  open_doors(target, 32);

  const auto edge = target.links.field(target.links.find_field("edge"));
  const auto kind = target.zones.field(target.zones.find_field("kind"));
  std::vector<size_t> used(place_count(target), 0);
  for (size_t i = 0; i < house_link_count(target); ++i) {
    ++used[size_t(edge.get(i, 0))];
    ++used[size_t(edge.get(i, 1))];
  }
  size_t over = 0;
  for (size_t zone = 0; zone < zone_count(target); ++zone) {
    if (kind.get(zone) != double(room_kind)) continue;
    over += size_t(used[zone] > 1);
  }
  CHECK(over == 0);
  CHECK(house_components(target) == 1);
}

// ОБЯЗАТЕЛЬНЫЕ ДВЕРИ И РОСТ ОТ ЦИРКУЛЯЦИИ.
//
// Требование — не запрет, и вторым первое не выражается: «служебный вход в кухню со двора» никаким
// «нельзя» не получить. Отсюда третья матрица и отдельная фаза, идущая ПЕРВОЙ.

namespace {
// Отдельная сцена: к дому пристроена улица — такая же зона, просто снаружи. Наружная дверь после
// этого не требует никакого нового механизма, и проверяется здесь именно это.
//
house_scene make_house_with_street(const int64_t street_depth = 4) {
  auto target = make_house();

  // Дом стоит НА УЧАСТКЕ: полоса улицы сверху, пятно застройки под ней.
  const auto* tool = registry().find("slice_zones");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("width", double(house_side));
  params.set_number("height", double(int64_t(house_side) - street_depth));
  params.set_number("origin_y", double(street_depth));
  params.set_number("wall", 1.0);
  params.set_number("corridor", 2.0);
  params.set_number("min_band", 4.0);
  params.set_number("max_band", 7.0);
  params.set_number("min_room", 3.0);
  params.set_number("max_room", 7.0);
  params.set_number("corridor_kind", double(corridor_kind));
  params.set_number("room_kind", double(room_kind));
  const std::vector<originator::field_ref> outputs{writable(target.zones, "rect"), writable(target.zones, "kind"),
                                                   writable(target.state, "zones")};
  originator::dispatch(*tool, {}, outputs, params, 20260912, 0, zone_capacity, "layout", nullptr);

  // Улицу кладёт площадка: раздел знает, как делить пятно, и ничего не знает про то, что снаружи.
  const auto count = zone_count(target);
  auto rect = target.zones.field(target.zones.find_field("rect"));
  auto kind = target.zones.field(target.zones.find_field("kind"));
  rect.set(count, 0.0, 0);
  rect.set(count, 0.0, 1);
  rect.set(count, double(house_side), 2);
  rect.set(count, double(street_depth), 3);
  kind.set(count, double(street_kind));
  target.state.field(target.state.find_field("zones")).set(0, double(count + 1));

  project(target, count + 1);
  return target;
}
} // namespace

TEST_CASE("originator slice_zones puts the building where the plot says, not in the corner") {
  auto target = make_house_with_street();
  const auto zones = read_zones(target, zone_count(target));

  // Все места дома ниже полосы улицы, и ни одно в неё не залезло: начало пятна — параметр, а не
  // молчаливый ноль.
  for (size_t i = 0; i + 1 < zones.size(); ++i) {
    CHECK(zones[i].y >= 4);
  }
  CHECK(zones.back().kind == street_kind);
}

TEST_CASE("originator open_doors opens a required door and marks the outer one as ordinary") {
  auto target = make_house_with_street();
  find_joins(target);

  // Улице дверь в место разрешена — и ОБЯЗАНА быть. Без требования наружная дверь зависела бы от
  // того, какой стык оказался длиннее.
  auto allowed = target.rules.field(target.rules.find_field("allowed"));
  allowed.set(street_kind * kind_count + room_kind, 1.0);
  allowed.set(room_kind * kind_count + street_kind, 1.0);

  const std::vector<field_pair> rule_fields = {{"allowed", "ub1"}};
  originator::buffer required(
    "required_doors", originator::make_buffer_layout(originator::storage_kind::soa, rule_fields, "required_doors"),
    kind_count * kind_count);
  auto needed = required.field(required.find_field("allowed"));
  needed.set(street_kind * kind_count + room_kind, 1.0);
  needed.set(room_kind * kind_count + street_kind, 1.0);

  const auto* tool = registry().find("open_doors");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("wall", 1.0);
  params.set_number("door_width", 1.0);
  params.set_number("door_kind", double(corridor_kind));
  const std::vector<originator::field_ref> inputs{
    readable(target.joins, "pair"),  readable(target.joins, "span"),  readable(target.state, "joins"),
    readable(target.zones, "kind"),  readable(target.state, "zones"), readable(target.rules, "allowed"),
    readable(target.kinds, "max_links"), readable(required, "allowed")};
  const std::vector<originator::field_ref> outputs{
    writable(target.zones, "rect"),   writable(target.zones, "kind"),  writable(target.links, "edge"),
    writable(target.state, "places"), writable(target.state, "links")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, zone_capacity, "doors", nullptr);

  const auto edge = target.links.field(target.links.find_field("edge"));
  const auto kind = target.zones.field(target.zones.find_field("kind"));

  // КАЖДЫЙ стык улицы с местом стал дверью — это и означает «обязана».
  const auto pair = target.joins.field(target.joins.find_field("pair"));
  const auto joins = size_t(target.state.field(target.state.find_field("joins")).get(0));
  size_t required_joins = 0;
  size_t opened = 0;
  for (size_t i = 0; i < joins; ++i) {
    const auto a = size_t(pair.get(i, 0));
    const auto b = size_t(pair.get(i, 1));
    // Считаются только те стыки, чья ПАРА видов объявлена обязательной: у улицы есть стык и с
    // коридором, но дверь туда не разрешена, и требовать её никто не требовал.
    const bool street_room = (kind.get(a) == double(street_kind) && kind.get(b) == double(room_kind)) ||
                             (kind.get(b) == double(street_kind) && kind.get(a) == double(room_kind));
    if (!street_room || pair.get(i, 2) == 0.0) continue;
    ++required_joins;
    for (size_t k = 0; k < house_link_count(target); ++k) {
      const auto la = size_t(edge.get(k, 0));
      const auto lb = size_t(edge.get(k, 1));
      opened += size_t((la == a && lb == b) || (la == b && lb == a));
    }
  }
  CHECK(required_joins > 0);
  CHECK(opened == required_joins);
  CHECK(house_components(target) == 1);
}

TEST_CASE("originator open_doors refuses a pair declared both required and forbidden") {
  auto target = make_house_with_street();
  find_joins(target);

  const std::vector<field_pair> rule_fields = {{"allowed", "ub1"}};
  originator::buffer required(
    "required_doors", originator::make_buffer_layout(originator::storage_kind::soa, rule_fields, "required_doors"),
    kind_count * kind_count);
  auto needed = required.field(required.find_field("allowed"));
  // Улице дверь в место НЕ разрешена (матрица дверей её не содержит), но объявлена обязательной.
  needed.set(street_kind * kind_count + room_kind, 1.0);
  needed.set(room_kind * kind_count + street_kind, 1.0);

  const auto* tool = registry().find("open_doors");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("wall", 1.0);
  params.set_number("door_kind", double(corridor_kind));
  const std::vector<originator::field_ref> inputs{
    readable(target.joins, "pair"),  readable(target.joins, "span"),  readable(target.state, "joins"),
    readable(target.zones, "kind"),  readable(target.state, "zones"), readable(target.rules, "allowed"),
    readable(target.kinds, "max_links"), readable(required, "allowed")};
  const std::vector<originator::field_ref> outputs{
    writable(target.zones, "rect"),   writable(target.zones, "kind"),  writable(target.links, "edge"),
    writable(target.state, "places"), writable(target.state, "links")};

  // Автор сказал две вещи сразу, и выбирать за него нельзя.
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, params, 1, 0, zone_capacity, "doors", nullptr),
                  std::exception);
}

TEST_CASE("originator open_doors grows the tree from the circulation, not from the longest joins") {
  // Тесный предел связей у мест — ровно тот случай, на котором жадный набор по длине разваливается:
  // место успевает потратить предел на соседние места и не добирается до коридора, а кусок дома
  // остаётся отрезанным. Рост от уже связанного этого не делает.
  auto target = make_house();
  slice(target, 20260912);
  project(target, zone_count(target));
  find_joins(target);

  auto allowed = target.rules.field(target.rules.find_field("allowed"));
  allowed.set(room_kind * kind_count + room_kind, 1.0); // место с местом теперь тоже можно
  target.kinds.field(target.kinds.find_field("max_links")).set(room_kind, 1.0);

  open_doors(target);
  CHECK(house_components(target) == 1);

  // И каждое место висит на циркуляции, а не на соседе: предел в одну связь другого не допускает.
  const auto edge = target.links.field(target.links.find_field("edge"));
  const auto kind = target.zones.field(target.zones.find_field("kind"));
  size_t room_to_room = 0;
  for (size_t i = 0; i < house_link_count(target); ++i) {
    room_to_room += size_t(kind.get(size_t(edge.get(i, 0))) == double(room_kind) &&
                           kind.get(size_t(edge.get(i, 1))) == double(room_kind));
  }
  CHECK(room_to_room == 0);
}

// ФОРМА МЕСТА, КОЛОННАДА И НЕРАЗРЕЗАННЫЕ ЧАСТИ.
//
// Постройка прямоугольниками не исчерпывается: угол срезают или скругляют, зал делают округлым, а
// крупный кусок не режут вовсе. Проверяется здесь не «стало красиво», а два обещания, которые только
// и можно предъявить: форма МЕНЯЕТ место, а не разрывает его, и колонна — это кусок стены, стоящий
// ОТДЕЛЬНО (иначе это выступ, а не колонна).

namespace {
// Сколько клеток у места и из скольких кусков оно состоит. Второе — то самое обещание формы.
struct zone_cells {
  size_t count = 0;
  size_t pieces = 0;
};

zone_cells measure_zone(const house_scene& target, const size_t zone) {
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  const size_t total = target.cells.count();
  std::vector<uint8_t> seen(total, 0);
  zone_cells result;

  for (size_t start = 0; start < total; ++start) {
    if (cells.get(start) != double(zone + 1) || seen[start] != 0) {
      continue;
    }
    ++result.pieces;
    std::vector<size_t> stack{start};
    seen[start] = 1;
    while (!stack.empty()) {
      const auto index = stack.back();
      stack.pop_back();
      ++result.count;
      const int64_t x = int64_t(index % house_side);
      const int64_t y = int64_t(index / house_side);
      const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& step : steps) {
        const int64_t nx = x + step[0];
        const int64_t ny = y + step[1];
        if (nx < 0 || ny < 0 || nx >= int64_t(house_side) || ny >= int64_t(house_side)) continue;
        const auto next = size_t(ny) * house_side + size_t(nx);
        if (seen[next] != 0 || cells.get(next) != double(zone + 1)) continue;
        seen[next] = 1;
        stack.push_back(next);
      }
    }
  }
  return result;
}

// Сцена из одного места объявленной формы: так проверяется сама форма, без раздела и ролей.
house_scene make_shaped(const int64_t shape, const int64_t parameter, const int64_t w = 13,
                        const int64_t h = 11) {
  auto target = make_house();
  auto rect = target.zones.field(target.zones.find_field("rect"));
  auto kind = target.zones.field(target.zones.find_field("kind"));
  rect.set(0, 4.0, 0);
  rect.set(0, 4.0, 1);
  rect.set(0, double(w), 2);
  rect.set(0, double(h), 3);
  kind.set(0, double(room_kind));
  target.state.field(target.state.find_field("zones")).set(0, 1.0);

  // ФОРМА ЛЕЖИТ РЯДОМ С ГАБАРИТОМ и после разметки никуда не девается: растр больше не истина о
  // клетках, и спросить у него, какой формы место, нельзя — форму ВЫЧИСЛЯЮТ там, где она нужна.
  auto field = target.shapes.field(target.shapes.find_field("shape"));
  field.set(0, double(shape), 0);
  field.set(0, double(parameter), 1);

  const auto* tool = registry().find("paint_rects");
  REQUIRE(tool != nullptr);
  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(target.shapes, "shape")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "zone")};
  originator::dispatch(*tool, inputs, outputs, empty, 1, 0, 1, "paint", nullptr);
  return target;
}
} // namespace

TEST_CASE("originator paint_rects shapes a place without tearing it apart") {
  const int64_t w = 13;
  const int64_t h = 11;

  struct variant {
    int64_t shape = 0;
    int64_t parameter = 0;
    const char* name = "";
  };
  const variant variants[] = {{0, 0, "rectangle"}, {1, 3, "chamfer"}, {2, 4, "rounded"}, {3, 0, "ellipse"}};

  for (const auto& item : variants) {
    CAPTURE(item.name);
    auto target = make_shaped(item.shape, item.parameter, w, h);
    const auto measured = measure_zone(target, 0);

    // ОДИН КУСОК — единственное, что форма обязана сохранить.
    CHECK(measured.pieces == 1);
    CHECK(measured.count > 0);
    CHECK(measured.count <= size_t(w * h));
    if (item.shape != 0) {
      CHECK(measured.count < size_t(w * h)); // форма обязана что-то срезать, иначе это прямоугольник
    }

    // Клетки не вылезают за габарит: форма живёт ВНУТРИ объявленного прямоугольника.
    const auto cells = target.cells.field(target.cells.find_field("zone"));
    for (size_t i = 0; i < target.cells.count(); ++i) {
      if (cells.get(i) == 0.0) continue;
      const int64_t x = int64_t(i % house_side);
      const int64_t y = int64_t(i / house_side);
      CHECK(x >= 4);
      CHECK(y >= 4);
      CHECK(x < 4 + w);
      CHECK(y < 4 + h);
    }
  }
}

TEST_CASE("originator paint_rects keeps a shape symmetric, because a lopsided hall reads as a mistake") {
  const int64_t w = 13;
  const int64_t h = 11;
  for (const int64_t shape : {int64_t(1), int64_t(2), int64_t(3)}) {
    auto target = make_shaped(shape, 4, w, h);
    const auto cells = target.cells.field(target.cells.find_field("zone"));
    const auto at = [&](const int64_t dx, const int64_t dy) {
      return cells.get(size_t(4 + dy) * house_side + size_t(4 + dx)) != 0.0;
    };
    for (int64_t dy = 0; dy < h; ++dy) {
      for (int64_t dx = 0; dx < w; ++dx) {
        // Симметрия по обеим осям: она следует из целочисленного счёта от центра, и если однажды
        // перестанет следовать, это будет видно здесь, а не на карте.
        CHECK(at(dx, dy) == at(w - 1 - dx, dy));
        CHECK(at(dx, dy) == at(dx, h - 1 - dy));
      }
    }
  }
}

TEST_CASE("originator paint_rects refuses a corner that eats the place instead of shaping it") {
  auto target = make_house();
  auto rect = target.zones.field(target.zones.find_field("rect"));
  rect.set(0, 4.0, 0);
  rect.set(0, 4.0, 1);
  rect.set(0, 8.0, 2);
  rect.set(0, 6.0, 3);

  const std::vector<field_pair> shape_fields = {{"shape", "ui2"}};
  originator::buffer shapes(
    "shapes", originator::make_buffer_layout(originator::storage_kind::soa, shape_fields, "shapes"), zone_capacity);
  auto field = shapes.field(shapes.find_field("shape"));
  field.set(0, 1.0, 0);
  field.set(0, 5.0, 1); // срез больше половины короткой стороны

  const auto* tool = registry().find("paint_rects");
  REQUIRE(tool != nullptr);
  const originator::parameters empty;
  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(shapes, "shape")};
  const std::vector<originator::field_ref> outputs{writable(target.cells, "zone")};
  CHECK_THROWS_AS(originator::dispatch(*tool, inputs, outputs, empty, 1, 0, 1, "paint", nullptr), std::exception);
}

TEST_CASE("originator paint_rects refuses to write one place over another") {
  auto target = make_house();
  auto rect = target.zones.field(target.zones.find_field("rect"));
  const auto place = [&](const size_t i, const double x, const double y) {
    rect.set(i, x, 0);
    rect.set(i, y, 1);
    rect.set(i, 6.0, 2);
    rect.set(i, 6.0, 3);
  };
  place(0, 4.0, 4.0);
  place(1, 8.0, 4.0); // перекрывается с первым

  // Перекрытие на растре НЕВИДИМО: второе место просто затирает первое, и карта выглядит обычной.
  // Отказывать обязан тот, кто теряет данные.
  CHECK_THROWS_AS(paint(target, 0, 2), std::exception);
}

TEST_CASE("originator place_columns leaves the hall whole and every column standing apart") {
  auto target = make_shaped(0, 0, 15, 13);

  const std::vector<field_pair> mask_fields = {{"columns", "ub1"}};
  originator::buffer mask(
    "mask", originator::make_buffer_layout(originator::storage_kind::soa, mask_fields, "mask"), zone_capacity);
  mask.field(mask.find_field("columns")).set(0, 1.0);

  const auto* tool = registry().find("place_columns");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("spacing", 4.0);
  params.set_number("size", 1.0);
  params.set_number("margin", 2.0);

  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(target.state, "zones"),
                                                  readable(target.shapes, "shape"), readable(mask, "columns")};
  const std::vector<originator::field_ref> outputs{writable(target.columns, "rect"),
                                                   writable(target.state, "columns")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, zone_capacity, "columns", nullptr);

  const auto placed = size_t(target.state.field(target.state.find_field("columns")).get(0));
  CHECK(placed > 0);

  // КОЛОННА ПОПАДАЕТ В КАРТИНКУ ВТОРЫМ ВХОДОМ ПРОЕКТОРА: сначала места, потом дырки в них. Сама она
  // клеток не пишет вовсе — и именно поэтому колоннада не поедет при смене масштаба.
  project(target, 1);

  // 1. ЗАЛ ОСТАЛСЯ ЦЕЛЫМ. Колоннада обязана быть препятствием, а не перегородкой.
  const auto measured = measure_zone(target, 0);
  CHECK(measured.pieces == 1);
  CHECK(measured.count == size_t(15 * 13) - placed);

  // 2. КАЖДАЯ КОЛОННА СТОИТ ОТДЕЛЬНО: у неё по всем четырём сторонам зал, а не другая колонна и не
  //    стена. Иначе это выступ стены, а не колонна.
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  size_t columns_seen = 0;
  for (int64_t y = 4; y < 4 + 13; ++y) {
    for (int64_t x = 4; x < 4 + 15; ++x) {
      if (cells.get(size_t(y) * house_side + size_t(x)) != 0.0) continue;
      ++columns_seen;
      const int64_t steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& step : steps) {
        CHECK(cells.get(size_t(y + step[1]) * house_side + size_t(x + step[0])) == 1.0);
      }
    }
  }
  CHECK(columns_seen == placed);
}

TEST_CASE("originator place_columns keeps out of the places it was not asked about") {
  auto target = make_shaped(0, 0, 15, 13);

  // Признак колоннады — ноль: место есть, а колонн в нём быть не должно.
  const std::vector<field_pair> mask_fields = {{"columns", "ub1"}};
  originator::buffer mask(
    "mask", originator::make_buffer_layout(originator::storage_kind::soa, mask_fields, "mask"), zone_capacity);

  const auto* tool = registry().find("place_columns");
  REQUIRE(tool != nullptr);
  originator::parameters params;
  params.set_number("spacing", 4.0);

  const std::vector<originator::field_ref> inputs{readable(target.zones, "rect"), readable(target.state, "zones"),
                                                  readable(target.shapes, "shape"), readable(mask, "columns")};
  const std::vector<originator::field_ref> outputs{writable(target.columns, "rect"),
                                                   writable(target.state, "columns")};
  originator::dispatch(*tool, inputs, outputs, params, 1, 0, zone_capacity, "columns", nullptr);

  CHECK(target.state.field(target.state.find_field("columns")).get(0) == 0.0);
  project(target, 1);
  CHECK(measure_zone(target, 0).count == size_t(15 * 13));
}

TEST_CASE("originator slice_zones leaves declared parts whole instead of cutting them") {
  const auto* tool = registry().find("slice_zones");
  REQUIRE(tool != nullptr);

  const auto layout = [&](const int64_t percent) {
    auto target = make_house();
    originator::parameters params;
    params.set_number("width", double(house_side));
    params.set_number("height", double(house_side));
    params.set_number("wall", 1.0);
    params.set_number("corridor", 2.0);
    params.set_number("min_band", 4.0);
    params.set_number("max_band", 7.0);
    params.set_number("min_room", 3.0);
    params.set_number("max_room", 7.0);
    params.set_number("reserve_percent", double(percent));
    params.set_number("corridor_kind", double(corridor_kind));
    params.set_number("room_kind", double(room_kind));
    const std::vector<originator::field_ref> outputs{writable(target.zones, "rect"), writable(target.zones, "kind"),
                                                     writable(target.state, "zones")};
    originator::dispatch(*tool, {}, outputs, params, 20260912, 0, zone_capacity, "layout", nullptr);
    return target;
  };

  auto cut = layout(0);
  auto whole = layout(100);

  const auto places = [&](const house_scene& target) {
    size_t rooms = 0;
    const auto zones = read_zones(target, zone_count(target));
    for (const auto& zone : zones) {
      rooms += size_t(zone.kind == room_kind);
    }
    return rooms;
  };

  // КРУПНЫЙ КУСОК НЕ РЕЖУТ, а не «режут на большие комнаты»: резка стремится к среднему, и зала из
  // неё не выходит. При полной доле мест становится ровно столько, сколько полос.
  CHECK(places(whole) < places(cut));
  const auto zones = read_zones(whole, zone_count(whole));
  int64_t widest = 0;
  for (const auto& zone : zones) {
    if (zone.kind == room_kind) widest = std::max(widest, zone.w);
  }
  CHECK(widest > 7); // шире объявленного предела комнаты: это уже не комната, а неразрезанная часть
}

// ============================== ПЛАН И КАРТИНКА ==============================
//
// Здесь проверяется единственное, ради чего план переезжал в меру: КАРТИНКА НИ НА ЧТО НЕ ВЛИЯЕТ.
// Утверждение это отрицательное, и потому проверяется тремя положительными: при масштабе один к
// одному картинка совпадает с планом; при сжатии она ГОВОРИТ, чего потеряла; при смещении окна она
// смещается ровно на объявленное, клетка в клетку.

TEST_CASE("originator project_zones draws the plan one to one, and paints it exactly as paint_rects would") {
  auto target = make_house();
  slice(target, 20260912);
  const auto zones = zone_count(target);
  REQUIRE(zones > 8);

  project(target, zones);
  CHECK(lost_places(target) == 0);
  CHECK(merged_borders(target) == 0);

  auto cells = target.cells.field(target.cells.find_field("zone"));
  std::vector<double> projected(target.cells.count());
  for (size_t i = 0; i < projected.size(); ++i) {
    projected[i] = cells.get(i);
    cells.set(i, 0.0);
  }

  // ДВА ПИСАТЕЛЯ ОДНОЙ КАРТИНКИ обязаны сойтись при масштабе один к одному, и это не проверка
  // «инструмент равен инструменту»: `paint_rects` разметку клеток и есть, а `project_zones` — это
  // выборка в центре клетки. Совпасть они обязаны ровно потому, что при равных сторонах центр клетки
  // попадает в ту же меру, что и её номер; разойдись они на клетку — и все проверки по растру
  // поехали бы на ту же клетку.
  paint(target, 0, zones);
  for (size_t i = 0; i < projected.size(); ++i) {
    CHECK(cells.get(i) == projected[i]);
  }
}

TEST_CASE("originator project_zones says what a coarser view loses instead of lying quietly") {
  auto target = make_house();
  slice(target, 20260912);
  const auto zones = zone_count(target);

  // Окно втрое шире картинки — то есть сжатие втрое. Стена в одну меру такого не переживает, и
  // молчаливая картинка показала бы здание без стен как здание с проходами.
  project(target, zones, 0, 0, int64_t(house_side) * 3, int64_t(house_side) * 3);
  CHECK(merged_borders(target) > 0);

  // А при масштабе один к одному тот же план теряется НУЛЁМ: огрубление — свойство показа, а не
  // плана, и число обязано это различать.
  project(target, zones);
  CHECK(lost_places(target) == 0);
  CHECK(merged_borders(target) == 0);
}

TEST_CASE("originator project_zones moves the window by exactly what was asked") {
  auto target = make_house();
  slice(target, 20260912);
  const auto zones = zone_count(target);

  project(target, zones);
  const auto cells = target.cells.field(target.cells.find_field("zone"));
  std::vector<double> whole(target.cells.count());
  for (size_t i = 0; i < whole.size(); ++i) {
    whole[i] = cells.get(i);
  }

  const int64_t shift_x = 7;
  const int64_t shift_y = 5;
  project(target, zones, shift_x, shift_y);
  for (int64_t y = 0; y + shift_y < int64_t(house_side); ++y) {
    for (int64_t x = 0; x + shift_x < int64_t(house_side); ++x) {
      CHECK(cells.get(size_t(y) * house_side + size_t(x)) ==
            whole[size_t(y + shift_y) * house_side + size_t(x + shift_x)]);
    }
  }
}

TEST_CASE("originator zone_joins measures the run along the shape, not along the bounding box") {
  auto target = make_house();
  auto rect = target.zones.field(target.zones.find_field("rect"));
  auto kind = target.zones.field(target.zones.find_field("kind"));
  auto shape = target.shapes.field(target.shapes.find_field("shape"));

  // Два места, между ними ровно стена в одну меру. Габариты перекрываются по всей стороне.
  const auto put = [&](const size_t index, const int64_t x, const int64_t y, const int64_t w, const int64_t h) {
    rect.set(index, double(x), 0);
    rect.set(index, double(y), 1);
    rect.set(index, double(w), 2);
    rect.set(index, double(h), 3);
    kind.set(index, double(room_kind));
  };
  put(0, 4, 4, 10, 10);
  put(1, 15, 4, 10, 10);
  target.state.field(target.state.find_field("zones")).set(0, 2.0);

  const auto only_join = [&]() {
    find_joins(target);
    const auto count = size_t(target.state.field(target.state.find_field("joins")).get(0));
    REQUIRE(count == 1);
    return int64_t(target.joins.field(target.joins.find_field("span")).get(0, 2));
  };

  // Прямоугольники: пробег во всю общую сторону.
  CHECK(only_join() == 10);

  // СРЕЗАННЫЙ УГОЛ КОРОЧЕ ГАБАРИТА, и стык обязан это заметить: дверь ставится в кромку, а срез —
  // это не кромка. Прежде так выходило само собой, потому что стыки читались с растра, который форму
  // знал; при переезде в меру это пришлось бы потерять, если бы пробег мерили по габариту.
  shape.set(0, double(1), 0); // chamfer
  shape.set(0, double(3), 1);
  CHECK(only_join() == 4);
}
