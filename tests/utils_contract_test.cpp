#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "devils_engine/utils/actor_ref.h"
#include "devils_engine/utils/event_dispatcher.h"
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
