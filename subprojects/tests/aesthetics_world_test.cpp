#include <algorithm>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/aesthetics/world.h"

using namespace devils_engine;

namespace {

struct position {
  int x = 0;
};

struct velocity {
  int x = 0;
};

struct marker {
  int value = 0;
};

template <typename View_T>
std::vector<aesthetics::entityid_t> collect_ids(const View_T& view) {
  std::vector<aesthetics::entityid_t> ids;
  for (const auto& tuple : view) {
    ids.push_back(std::get<0>(tuple));
  }
  std::sort(ids.begin(), ids.end(), [](const auto a, const auto b) {
    return aesthetics::get_entityid_index(a) < aesthetics::get_entityid_index(b);
  });
  return ids;
}

} // namespace

TEST_CASE("world view iterates sparse storage without a separate dense entity list [aesthetics::world]") {
  aesthetics::world world;
  const auto a = world.gen_entityid();
  const auto b = world.gen_entityid();
  const auto c = world.gen_entityid();

  REQUIRE(world.create<position>(a, 10) != nullptr);
  REQUIRE(world.create<position>(b, 20) != nullptr);
  REQUIRE(world.create<position>(c, 30) != nullptr);
  REQUIRE(world.count<position>() == 3);

  CHECK(world.remove<position>(b));
  CHECK(world.count<position>() == 2);
  REQUIRE(world.get<position>(a) != nullptr);
  REQUIRE(world.get<position>(c) != nullptr);
  CHECK(world.get<position>(a)->x == 10);
  CHECK(world.get<position>(c)->x == 30);
  CHECK(world.get<position>(b) == nullptr);

  const auto ids = collect_ids(world.view<position>());
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == a);
  CHECK(ids[1] == c);
}

TEST_CASE("world reconstructs raw iterator entity versions from sparse storage [aesthetics::world]") {
  aesthetics::world world;
  const auto old_id = world.gen_entityid();

  REQUIRE(world.create<position>(old_id, 1) != nullptr);
  world.remove_entity(old_id);

  const auto new_id = world.gen_entityid();
  REQUIRE(aesthetics::get_entityid_index(new_id) == aesthetics::get_entityid_index(old_id));
  REQUIRE(aesthetics::get_entityid_version(new_id) == aesthetics::get_entityid_version(old_id) + 1);
  REQUIRE(world.create<position>(new_id, 2) != nullptr);

  CHECK(world.get<position>(old_id) == nullptr);
  REQUIRE(world.get<position>(new_id) != nullptr);
  CHECK(world.get<position>(new_id)->x == 2);

  const auto ids = collect_ids(world.view<position>());
  REQUIRE(ids.size() == 1);
  CHECK(ids[0] == new_id);
}

TEST_CASE("const world views allow component mutation without changing world membership [aesthetics::world]") {
  aesthetics::world world;
  const auto id = world.gen_entityid();

  REQUIRE(world.create<position>(id, 1) != nullptr);

  const auto& const_world = world;
  for (auto [entity_id, pos] : const_world.view<position>()) {
    CHECK(entity_id == id);
    REQUIRE(pos != nullptr);
    pos->x = 5;
  }

  REQUIRE(world.get<position>(id) != nullptr);
  CHECK(world.get<position>(id)->x == 5);

  for (auto [entity_id, pos] : const_world.lazy_view<position>()) {
    CHECK(entity_id == id);
    REQUIRE(pos != nullptr);
    pos->x = 9;
  }

  REQUIRE(world.get<position>(id) != nullptr);
  CHECK(world.get<position>(id)->x == 9);
  CHECK(world.count<position>() == 1);
}

TEST_CASE("query follows component create and remove events [aesthetics::world]") {
  aesthetics::world world;
  const auto a = world.gen_entityid();
  const auto b = world.gen_entityid();

  REQUIRE(world.create<position>(a, 1) != nullptr);
  REQUIRE(world.create<velocity>(a, 10) != nullptr);
  REQUIRE(world.create<position>(b, 2) != nullptr);

  auto query = world.query<position, velocity>();
  REQUIRE(query.size() == 1);
  CHECK(std::get<0>(query[0]) == a);

  REQUIRE(world.create<velocity>(b, 20) != nullptr);
  CHECK(query.size() == 2);

  CHECK(world.remove<position>(a));
  REQUIRE(query.size() == 1);
  CHECK(std::get<0>(query[0]) == b);
}

TEST_CASE("lazy_query removes an entity when the last matching component is removed [aesthetics::world]") {
  aesthetics::world world;
  const auto a = world.gen_entityid();
  const auto b = world.gen_entityid();

  REQUIRE(world.create<position>(a, 1) != nullptr);
  REQUIRE(world.create<position>(b, 2) != nullptr);
  REQUIRE(world.create<marker>(b, 3) != nullptr);

  auto query = world.lazy_query<position, marker>();
  REQUIRE(query.size() == 2);

  CHECK(world.remove<position>(a));
  REQUIRE(query.size() == 1);
  CHECK(std::get<0>(query[0]) == b);

  CHECK(world.remove<position>(b));
  REQUIRE(query.size() == 1);
  CHECK(std::get<0>(query[0]) == b);

  CHECK(world.remove<marker>(b));
  CHECK(query.size() == 0);
}
