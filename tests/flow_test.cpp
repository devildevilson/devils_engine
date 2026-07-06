#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "devils_engine/demiurg/module_system.h"
#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/flow/system.h"
#include "devils_engine/flow/animation_resource.h"
#include "devils_engine/utils/safe_handle.h"

using namespace devils_engine;

TEST_CASE("flow directional buckets are centered at zero") {
  constexpr float pi = 3.14159265358979323846f;

  CHECK(flow::directional_image_index(0.0f, 8) == 0);
  CHECK(flow::directional_image_index((pi / 8.0f) * 0.99f, 8) == 0);
  CHECK(flow::directional_image_index((pi / 8.0f) * 1.01f, 8) == 1);
  CHECK(flow::directional_image_index(-(pi / 8.0f) * 0.99f, 8) == 0);
  CHECK(flow::directional_image_index(-(pi / 8.0f) * 1.01f, 8) == 7);
}

TEST_CASE("flow playback emits actions once and advances states") {
  flow::library lib;

  flow::state a;
  a.duration_mcs = 100;
  a.next = 1;
  a.action = utils::string_hash("scripts/start");
  a.uv = {0.25f, 0.0f};
  a.images.push_back({});

  flow::state b;
  b.duration_mcs = 100;
  b.action = utils::string_hash("scripts/end");
  b.uv = {0.0f, 0.5f};
  b.images.push_back({});

  lib.add_state("anim/a:0", a);
  lib.add_state("anim/a:1", b);

  flow::playback pb;
  pb.current = 0;

  auto r0 = lib.sample(pb, 50, {});
  REQUIRE(r0.actions.size() == 1);
  CHECK(r0.actions[0].action == utils::string_hash("scripts/start"));
  CHECK(pb.current == 0);
  CHECK(pb.elapsed_mcs == 50);
  CHECK(pb.uv.x == doctest::Approx(0.125f));
  CHECK(pb.uv.y == doctest::Approx(0.0f));

  auto r1 = lib.sample(pb, 50, {});
  REQUIRE(r1.actions.size() == 1);
  CHECK(r1.actions[0].action == utils::string_hash("scripts/end"));
  CHECK(pb.current == 1);
  CHECK(pb.elapsed_mcs == 0);
  CHECK(pb.uv.x == doctest::Approx(0.25f));

  auto r2 = lib.sample(pb, 1, {});
  REQUIRE(r2.actions.empty());
  CHECK(pb.current == 1);
}

TEST_CASE("flow uv accumulates and truncates whole part") {
  flow::library lib;

  flow::state a;
  a.duration_mcs = 100;
  a.next = 0;
  a.uv = {0.75f, 1.25f};
  a.images.push_back({});

  lib.add_state("anim/uv:0", a);

  flow::playback pb;
  pb.current = 0;

  lib.sample(pb, 100, {});
  CHECK(pb.uv.x == doctest::Approx(0.75f));
  CHECK(pb.uv.y == doctest::Approx(0.25f));

  lib.sample(pb, 100, {});
  CHECK(pb.uv.x == doctest::Approx(0.5f));
  CHECK(pb.uv.y == doctest::Approx(0.5f));
}

TEST_CASE("flow zero-duration states chain actions with guard") {
  flow::library lib;

  flow::state a;
  a.duration_mcs = 0;
  a.next = 1;
  a.action = utils::string_hash("scripts/a");
  a.uv = {0.1f, 0.0f};

  flow::state b;
  b.duration_mcs = 0;
  b.next = 2;
  b.action = utils::string_hash("scripts/b");
  b.uv = {0.2f, 0.0f};

  flow::state c;
  c.duration_mcs = 100;
  c.action = utils::string_hash("scripts/c");
  c.images.push_back({});

  lib.add_state("anim/z:0", a);
  lib.add_state("anim/z:1", b);
  lib.add_state("anim/z:2", c);

  flow::playback pb;
  pb.current = 0;

  auto r = lib.sample(pb, 0, {});
  REQUIRE(r.actions.size() == 3);
  CHECK(r.actions[0].action == utils::string_hash("scripts/a"));
  CHECK(r.actions[1].action == utils::string_hash("scripts/b"));
  CHECK(r.actions[2].action == utils::string_hash("scripts/c"));
  CHECK(pb.current == 2);
  CHECK(pb.uv.x == doctest::Approx(0.3f));
}

TEST_CASE("flow parses state tavl") {
  const std::string text = R"(
{
  duration = 123
  next = "anim/abc:1"
  images = [ "tex/img", "tex/img:u", "tex/img2:3:uv" ]
  action = "scripts/attack"
  uv = [ 0.25, 0.5 ]
}

{
  duration = 0
  next = null
  images = []
  action = null
  uv = [ 0.0, 0.0 ]
}
)";

  std::vector<std::string> next;
  const auto states = flow::parse_state_text(text, "anim/abc", nullptr, &next);

  REQUIRE(states.size() == 2);
  REQUIRE(next.size() == 2);
  CHECK(next[0] == "anim/abc:1");
  CHECK(next[1].empty());
  CHECK(states[0].duration_mcs == 123);
  CHECK(states[0].images.size() == 3);
  CHECK(states[0].images[0].mirror_state == flow::mirror::none);
  CHECK(states[0].images[1].mirror_state == flow::mirror::u);
  CHECK(states[0].images[2].mirror_state == flow::mirror::uv);
  CHECK(states[0].action == utils::string_hash("scripts/attack"));
  CHECK(states[0].uv.x == doctest::Approx(0.25f));
  CHECK(states[0].uv.y == doctest::Approx(0.5f));
  CHECK(states[1].action == utils::invalid_id);
}

TEST_CASE("flow animation_resource uses demiurg tavl list subresources") {
  namespace fs = std::filesystem;

  const auto root = fs::temp_directory_path() / "devils_engine_flow_list_test";
  fs::remove_all(root);
  fs::create_directories(root / "core" / "anim");

  {
    std::ofstream out(root / "core" / "anim" / "walk.tavl");
    out << "name = idle\n";
    out << "duration = 100\n";
    out << "next = \"anim/walk:run\"\n";
    out << "images = []\n";
    out << "action = null\n";
    out << "uv = [ 0.0, 0.0 ]\n";
    out << "//---\n";
    out << "name = run\n";
    out << "duration = 200\n";
    out << "next = null\n";
    out << "images = []\n";
    out << "action = null\n";
    out << "uv = [ 0.0, 0.0 ]\n";
  }

  demiurg::module_system modules((root.generic_string() + "/"));
  modules.load_modules({demiurg::module_system::list_entry{"core/", "", ""}});

  flow::library lib;
  demiurg::resource_system resources;
  resources.register_type<flow::animation_resource>("anim", "tavl", &lib, &resources);
  resources.parse_resources(&modules);

  auto* idle = resources.get<flow::animation_resource>("anim/walk:idle");
  auto* run = resources.get<flow::animation_resource>("anim/walk:run");
  REQUIRE(idle != nullptr);
  REQUIRE(run != nullptr);
  CHECK(resources.get("anim/walk:0") == idle);
  CHECK(resources.get("anim/walk:1") == run);

  idle->load(utils::safe_handle_t(idle));
  run->load(utils::safe_handle_t(run));

  const uint32_t idle_index = lib.find_state("anim/walk:idle");
  const uint32_t run_index = lib.find_state("anim/walk:run");
  REQUIRE(idle_index != flow::invalid_state);
  REQUIRE(run_index != flow::invalid_state);
  REQUIRE(lib.get(idle_index) != nullptr);
  CHECK(lib.get(idle_index)->next == run_index);
  CHECK(idle->state_indices().size() == 1);
  CHECK(run->state_indices().size() == 1);

  fs::remove_all(root);
}
