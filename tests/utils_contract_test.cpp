#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "devils_engine/utils/actor_ref.h"
#include "devils_engine/utils/event_dispatcher.h"
#include "devils_engine/utils/memory_pool.h"
#include "devils_engine/utils/stack_allocator.h"
#include "devils_engine/utils/string_id.h"

using namespace devils_engine;

namespace {

struct actor_msg {
  int value;
};

class actor_msg_receiver final : public utils::message_reciever<actor_msg> {
public:
  std::vector<int> values;

  utils::send_status send(actor_msg msg) override {
    values.push_back(msg.value);
    return utils::send_status::ok;
  }

  utils::send_status send(std::vector<actor_msg>& msgs) override {
    for (const auto& msg : msgs) values.push_back(msg.value);
    msgs.clear();
    return utils::send_status::ok;
  }
};

struct event_a {
  int value;
};

struct event_b {
  std::string value;
};

struct pooled_node {
  static inline int alive_count = 0;

  int value;
  void* free_list_storage;

  explicit pooled_node(const int value_) noexcept : value(value_), free_list_storage(nullptr) { ++alive_count; }
  ~pooled_node() noexcept { --alive_count; }
};

struct alignas(64) over_aligned_node {
  std::uint64_t value;
  std::byte padding[64 - sizeof(std::uint64_t)];
};

}

TEST_CASE("actor_ref routes messages by actor and message type [utils::actor_ref]") {
  utils::actor_ref<1001> ref;
  actor_msg_receiver receiver;

  CHECK(ref.send(actor_msg{1}) == utils::send_status::no_receiver);

  ref.add_receiver<actor_msg>(&receiver);
  CHECK(ref.send(actor_msg{2}) == utils::send_status::ok);
  REQUIRE(receiver.values.size() == 1);
  CHECK(receiver.values[0] == 2);

  std::vector<actor_msg> batch = { actor_msg{3}, actor_msg{4} };
  CHECK(ref.send(batch) == utils::send_status::ok);
  CHECK(batch.empty());
  REQUIRE(receiver.values.size() == 3);
  CHECK(receiver.values[1] == 3);
  CHECK(receiver.values[2] == 4);
}

TEST_CASE("string_pool keeps stable dense ids and supports lookup [utils::string_id]") {
  utils::string_pool<1002> pool;

  CHECK(pool.empty());
  const auto grass = pool.reg("grass");
  const auto stone = pool.reg("stone");
  const auto grass_again = pool.reg("grass");

  CHECK(grass == 0);
  CHECK(stone == 1);
  CHECK(grass_again == grass);
  CHECK(pool.size() == 2);
  CHECK(pool.contains("stone"));
  CHECK(pool.lookup("missing") == utils::invalid_id);
  CHECK(pool.name(grass) == "grass");
  CHECK(pool.name(stone) == "stone");
  CHECK(pool.name(99).empty());

  pool.clear();
  CHECK(pool.empty());
  CHECK(pool.lookup("grass") == utils::invalid_id);
}

TEST_CASE("event_dispatcher2 stores independent typed event batches [utils::event_dispatcher2]") {
  utils::event_dispatcher2<1003> dispatcher;

  CHECK(dispatcher.read<event_a>().empty());

  dispatcher.submit(event_a{3}, event_a{1}, event_a{2});
  dispatcher.submit(std::vector<event_b>{ event_b{"first"}, event_b{"second"} });

  dispatcher.sort<event_a>([](const event_a& l, const event_a& r) { return l.value < r.value; });

  auto events_a = dispatcher.read<event_a>();
  REQUIRE(events_a.size() == 3);
  CHECK(events_a[0].value == 1);
  CHECK(events_a[1].value == 2);
  CHECK(events_a[2].value == 3);

  const auto events_b = dispatcher.consume<event_b>();
  REQUIRE(events_b.size() == 2);
  CHECK(events_b[0].value == "first");
  CHECK(events_b[1].value == "second");
  CHECK(dispatcher.read<event_b>().empty());

  dispatcher.clear_all();
  CHECK(dispatcher.read<event_a>().empty());
}

TEST_CASE("memory_pool allocates aligned objects and reuses destroyed slots [utils::memory_pool]") {
  pooled_node::alive_count = 0;
  utils::memory_pool<pooled_node, sizeof(pooled_node) * 8> pool;

  auto* first = pool.create(1);
  auto* second = pool.create(2);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->value == 1);
  CHECK(second->value == 2);
  CHECK(pooled_node::alive_count == 2);
  CHECK(pool.blocks_allocated() == 1);

  pool.destroy(first);
  CHECK(pooled_node::alive_count == 1);

  auto* reused = pool.create(3);
  CHECK(reused == first);
  CHECK(reused->value == 3);
  CHECK(pooled_node::alive_count == 2);

  pool.destroy(second);
  pool.destroy(reused);
  CHECK(pooled_node::alive_count == 0);
}

TEST_CASE("memory_pool reports the real per-block capacity [utils::memory_pool]") {
  utils::memory_pool<pooled_node, sizeof(pooled_node) * 4> pool;
  const size_t block_elem_count = pool.block_elem_count();
  REQUIRE(block_elem_count > 0);

  std::vector<pooled_node*> nodes;
  nodes.reserve(block_elem_count + 1);
  for (size_t i = 0; i < block_elem_count; ++i) {
    nodes.push_back(pool.create(static_cast<int>(i)));
  }

  CHECK(pool.blocks_allocated() == 1);
  nodes.push_back(pool.create(99));
  CHECK(pool.blocks_allocated() == 2);

  for (auto* node : nodes) pool.destroy(node);
}

TEST_CASE("memory_pool supports over-aligned object types [utils::memory_pool]") {
  utils::memory_pool<over_aligned_node, sizeof(over_aligned_node) * 2> pool;

  auto* node = pool.create();
  REQUIRE(node != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(node) % alignof(over_aligned_node) == 0);

  pool.destroy(node);
}

TEST_CASE("stack_allocator is linear, aligned, and reusable after clear [utils::stack_allocator]") {
  utils::stack_allocator allocator(64, 16);

  auto* first = allocator.allocate(1);
  auto* second = allocator.allocate(17);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(first) % 16 == 0);
  CHECK(reinterpret_cast<std::uintptr_t>(second) % 16 == 0);
  CHECK(allocator.size() == 48);

  CHECK(allocator.allocate(32) == nullptr);
  CHECK(allocator.size() == 48);

  allocator.clear();
  CHECK(allocator.size() == 0);
  CHECK(allocator.allocate(64) == allocator.data());
}

TEST_CASE("stack_allocator_mt does not consume capacity on failed allocation [utils::stack_allocator_mt]") {
  utils::stack_allocator_mt allocator(32, 16);

  REQUIRE(allocator.allocate(16) != nullptr);
  CHECK(allocator.allocated_size() == 16);

  CHECK(allocator.allocate(64) == nullptr);
  CHECK(allocator.allocated_size() == 16);

  REQUIRE(allocator.allocate(16) != nullptr);
  CHECK(allocator.allocated_size() == 32);
  CHECK(allocator.allocate(16) == nullptr);
  CHECK(allocator.allocated_size() == 32);
}

TEST_CASE("fixed_pool_mt returns nullptr when exhausted and reuses freed blocks [utils::fixed_pool_mt]") {
  utils::fixed_pool_mt pool(64, 16, 16);
  std::vector<void*> blocks;

  for (size_t i = 0; i < 4; ++i) {
    auto* block = pool.allocate();
    REQUIRE(block != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(block) % 16 == 0);
    blocks.push_back(block);
  }

  CHECK(pool.allocate() == nullptr);

  pool.free(blocks.back());
  blocks.pop_back();
  auto* reused = pool.allocate();
  REQUIRE(reused != nullptr);
  blocks.push_back(reused);

  for (auto* block : blocks) pool.free(block);
}
