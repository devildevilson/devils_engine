#include <cstdint>
#include <tuple>
#include <vector>

#include <doctest/doctest.h>

#include <algorithm>

#include <devils_engine/aesthetics/common.h>
#include <devils_engine/aesthetics/message_registry.h>
#include <devils_engine/aesthetics/template_system.h>
#include <devils_engine/aesthetics/world.h>
#include <devils_engine/aesthetics/worklist_system.h>
#include <devils_engine/thread/atomic_pool.h>

using namespace devils_engine;
using aesthetics::entityid_t;
using aesthetics::get_entityid_index;
using aesthetics::message_registry;

namespace {
struct heat {
  int32_t v = 0;
};
struct hot_msg {
  int32_t doubled = 0;
};
struct other_msg {
  int32_t tag = 0;
};
} // namespace

TEST_CASE("message_registry keeps independent channels per message type") {
  message_registry board;
  CHECK(board.find<hot_msg>() == nullptr);
  CHECK_FALSE(board.has<hot_msg>());

  auto& hot = board.channel<hot_msg>();
  auto& oth = board.channel<other_msg>();
  hot.reset(4);
  oth.reset(4);

  const auto e1 = aesthetics::make_entityid(1, 0);
  const auto e2 = aesthetics::make_entityid(2, 0);
  hot.store(e1, hot_msg{11});
  oth.store(e2, other_msg{22});

  CHECK(board.has<hot_msg>());
  CHECK(board.has<other_msg>());
  // Каналы не пересекаются: hot видит только e1, other — только e2.
  REQUIRE(board.find<hot_msg>() != nullptr);
  REQUIRE(board.find<other_msg>() != nullptr);
  CHECK(board.find<hot_msg>()->has(e1));
  CHECK_FALSE(board.find<hot_msg>()->has(e2));
  CHECK(board.find<other_msg>()->has(e2));
  CHECK_FALSE(board.find<other_msg>()->has(e1));

  // channel<Msg>() отдаёт тот же канал (ссылка стабильна).
  CHECK(&board.channel<hot_msg>() == &hot);
}

TEST_CASE("message_registry reset_all and clear_all sweep every channel") {
  message_registry board;
  board.channel<hot_msg>().reset(4);
  board.channel<other_msg>().reset(4);
  const auto e = aesthetics::make_entityid(1, 0);
  board.channel<hot_msg>().store(e, hot_msg{1});
  board.channel<other_msg>().store(e, other_msg{2});
  REQUIRE(board.channel<hot_msg>().size() == 1);
  REQUIRE(board.channel<other_msg>().size() == 1);

  board.clear_all();
  CHECK(board.channel<hot_msg>().empty());
  CHECK(board.channel<other_msg>().empty());
  CHECK(board.channel<hot_msg>().capacity() == 4); // ёмкость сохранена

  board.channel<hot_msg>().store(e, hot_msg{1});
  board.reset_all(16);
  CHECK(board.channel<hot_msg>().empty());
  CHECK(board.channel<hot_msg>().capacity() == 16); // reset_all сайзит все каналы
  CHECK(board.channel<other_msg>().capacity() == 16);
}

namespace {
// Продюсер: на каждую сущность с heat пишет в шину сообщение по СВОЕМУ слоту (индекс сущности) —
// непересекающиеся записи ⇒ параллельный обход без гонок.
struct doubler : aesthetics::template_system_mt<heat> {
  message_registry* board;
  doubler(thread::atomic_pool* pool, aesthetics::world* w, message_registry* b) noexcept
    : aesthetics::template_system_mt<heat>(pool, w), board(b) {}
  void process(const query_tuple_t& t, const size_t /*time*/) override {
    const auto id = std::get<0>(t);
    const auto* h = std::get<1>(t);
    board->channel<hot_msg>().store(id, hot_msg{h->v * 2});
  }
};
} // namespace

TEST_CASE("template_system_mt maps a query into a message_registry channel across threads") {
  constexpr int32_t n = 1500;
  aesthetics::world world;
  std::vector<entityid_t> ids;
  for (int32_t i = 0; i < n; ++i) {
    const auto id = world.gen_entityid();
    REQUIRE(world.create<heat>(id, heat{i}) != nullptr);
    ids.push_back(id);
  }

  message_registry board;
  board.channel<hot_msg>().reset(world.index_capacity()); // канал создаём+сайзим ДО параллельной фазы

  thread::atomic_pool pool(4);
  doubler sys(&pool, &world, &board); // query собирается над уже созданными сущностями
  REQUIRE(sys.size() == size_t(n));

  sys.update(0); // продюсер: параллельный map → шина

  // Консюмер: читает шину. Все n сообщений присутствуют, в порядке индекса, значение = heat*2.
  const auto* ch = board.find<hot_msg>();
  REQUIRE(ch != nullptr);
  CHECK(ch->size() == size_t(n));
  std::vector<std::pair<size_t, int32_t>> got;
  ch->for_each([&](const entityid_t id, const hot_msg& m) {
    got.push_back({get_entityid_index(id), m.doubled});
  });
  REQUIRE(got.size() == size_t(n));
  bool ordered_and_correct = true;
  for (int32_t i = 0; i < n; ++i) {
    ordered_and_correct = ordered_and_correct && got[i].first == size_t(i) && got[i].second == i * 2;
  }
  CHECK(ordered_and_correct);
}

TEST_CASE("budget_clamp keeps the highest-priority candidates with a deterministic tiebreak") {
  struct cand {
    uint64_t overdue;
    uint32_t id;
  };
  auto priority = [](const cand& a, const cand& b) {
    if (a.overdue != b.overdue) {
      return a.overdue > b.overdue; // дольше ждавшие приоритетнее
    }
    return a.id < b.id; // тай-брейк по индексу (детерминированно)
  };
  auto ids_of = [](std::vector<cand>& v) {
    std::vector<uint32_t> out;
    for (const auto& c : v) {
      out.push_back(c.id);
    }
    std::sort(out.begin(), out.end());
    return out;
  };

  SUBCASE("no-op when within budget") {
    std::vector<cand> due{{5, 0}, {3, 1}};
    devils_engine::aesthetics::budget_clamp(due, 4, priority);
    CHECK(due.size() == 2);
  }
  SUBCASE("clamps to the most overdue") {
    std::vector<cand> due;
    for (uint32_t i = 0; i < 10; ++i) {
      due.push_back({i + 10ull, i}); // overdue растёт с id ⇒ топ-4 = id 6..9
    }
    devils_engine::aesthetics::budget_clamp(due, 4, priority);
    REQUIRE(due.size() == 4);
    CHECK(ids_of(due) == std::vector<uint32_t>{6, 7, 8, 9});
  }
  SUBCASE("equal priority falls back to the index tiebreak") {
    std::vector<cand> due;
    for (uint32_t i = 0; i < 10; ++i) {
      due.push_back({50, i}); // одинаковый overdue ⇒ по индексу: наименьшие 0,1,2
    }
    devils_engine::aesthetics::budget_clamp(due, 3, priority);
    REQUIRE(due.size() == 3);
    CHECK(ids_of(due) == std::vector<uint32_t>{0, 1, 2});
  }
}

namespace {
// Consumer/think-система на worklist_system: обходит work-list по потокам, каждый — в свою scratch-
// полосу (int-счётчик), и пишет в шину сообщение по слоту сущности с прокинутым time.
struct think_msg {
  size_t index = 0;
  size_t time = 0;
};
struct thinker : aesthetics::worklist_system<int> {
  message_registry* board;
  thinker(thread::atomic_pool* pool, message_registry* b) noexcept
    : aesthetics::worklist_system<int>(pool), board(b) {}
  void process(const entityid_t id, int& scratch, const size_t time) override {
    scratch += 1; // per-thread scratch доступна и эксклюзивна
    board->channel<think_msg>().store(id, think_msg{get_entityid_index(id), time});
  }
};
} // namespace

TEST_CASE("worklist_system maps an explicit work-list across threads into a channel, threading time") {
  constexpr size_t n = 1200;
  thread::atomic_pool pool(4);
  message_registry board;

  thinker sys(&pool, &board);
  auto& wl = sys.worklist();
  for (size_t i = 0; i < n; ++i) {
    wl.push_back(aesthetics::make_entityid(i, uint32_t(i % 5)));
  }
  board.channel<think_msg>().reset(n); // канал сайзим ДО параллельной фазы

  sys.run(777); // time прокидывается в process

  const auto* ch = board.find<think_msg>();
  REQUIRE(ch != nullptr);
  CHECK(ch->size() == n);
  bool ok = true;
  ch->for_each([&](const entityid_t id, const think_msg& m) {
    ok = ok && m.index == get_entityid_index(id) && m.time == 777;
  });
  CHECK(ok);

  // scratch-полосы созданы под число потоков (pool.size()+1), суммарно инкрементов = n.
  size_t total = 0;
  for (const auto& s : sys.lanes()) {
    total += size_t(s);
  }
  CHECK(total == n);
}

TEST_CASE("worklist_system with an empty work-list is a no-op") {
  thread::atomic_pool pool(2);
  message_registry board;
  thinker sys(&pool, &board);
  sys.run(1);
  CHECK(sys.lanes().empty());     // полосы не аллоцируются на пустом прогоне
  CHECK_FALSE(board.has<think_msg>());
}
