#include <array>
#include <string_view>
#include <type_traits>

#include <doctest/doctest.h>

#include <devils_engine/painter/queue.h>
#include <devils_engine/painter/auxiliary.h>

using namespace devils_engine;

TEST_CASE("painter queue plan uses one queue from distinct role families") {
  const std::array<uint32_t, 3> counts = {1, 1, 1};
  const auto plan = painter::make_device_queue_plan(counts, 2, 0, 1);

  CHECK(plan.graphics == painter::queue_location{2, 0});
  CHECK(plan.transfer == painter::queue_location{0, 0});
  CHECK(plan.compute == painter::queue_location{1, 0});
  REQUIRE(plan.request_count == 3);
  CHECK(plan.requests[0].family == 0);
  CHECK(plan.requests[1].family == 1);
  CHECK(plan.requests[2].family == 2);
}

TEST_CASE("painter queue plan splits roles inside a universal family") {
  const std::array<uint32_t, 1> counts = {3};
  const auto plan = painter::make_device_queue_plan(counts, 0, 0, 0);

  CHECK(plan.graphics == painter::queue_location{0, 0});
  CHECK(plan.transfer == painter::queue_location{0, 1});
  CHECK(plan.compute == painter::queue_location{0, 2});
  REQUIRE(plan.request_count == 1);
  CHECK(plan.requests[0].count == 3);
}

TEST_CASE("painter queue plan aliases roles when only one queue exists") {
  const std::array<uint32_t, 1> counts = {1};
  const auto plan = painter::make_device_queue_plan(counts, 0, 0, 0);

  CHECK(plan.graphics == painter::queue_location{0, 0});
  CHECK(plan.transfer == painter::queue_location{0, 0});
  CHECK(plan.compute == painter::queue_location{0, 0});
  REQUIRE(plan.request_count == 1);
  CHECK(plan.requests[0].count == 1);
}

TEST_CASE("painter queue plan preserves transfer separation with two universal queues") {
  const std::array<uint32_t, 1> counts = {2};
  const auto plan = painter::make_device_queue_plan(counts, 0, 0, 0);

  CHECK(plan.graphics == painter::queue_location{0, 0});
  CHECK(plan.transfer == painter::queue_location{0, 1});
  CHECK(plan.compute == painter::queue_location{0, 0});
  CHECK(plan.requests[0].count == 2);
}

static_assert(!std::is_same_v<painter::graphics_queue, painter::transfer_queue>);
static_assert(!std::is_same_v<painter::graphics_queue, painter::compute_queue>);

TEST_CASE("painter debug utils extension is selected at runtime") {
  const auto plain = painter::get_required_extensions(false, false);
  const auto debug = painter::get_required_extensions(false, true);

  CHECK(plain.empty());
  REQUIRE(debug.size() == 1);
  CHECK(std::string_view(debug.front()) == "VK_EXT_debug_utils");
}
