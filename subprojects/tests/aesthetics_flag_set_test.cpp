#include <devils_engine/aesthetics/flag_set.h>
#include <devils_engine/utils/string_id.h>
#include <devils_engine/utils/timeline.h>
#include <doctest/doctest.h>

using namespace devils_engine;

namespace {
constexpr utils::game_duration seconds(const uint64_t s) {
  return utils::game_duration::from_seconds(s);
}
} // namespace

TEST_CASE("flag_set stores hashed flags with countdown expiry [aesthetics]") {
  aesthetics::flag_set flags;
  const auto stunned = utils::string_hash("stunned");
  const auto blessed = utils::string_hash("blessed");

  CHECK(flags.set(stunned, seconds(10)));
  CHECK(flags.set(blessed)); // бессрочный
  CHECK_FALSE(flags.set(stunned, seconds(20))); // refresh, не новая запись
  CHECK(flags.size() == 2);
  CHECK(flags.has(stunned));
  CHECK(flags.has(blessed));

  // advance вычитает игровую дельту; остаток виден через find.
  CHECK(flags.advance(seconds(15)) == 0);
  REQUIRE(flags.find(stunned) != nullptr);
  CHECK(flags.find(stunned)->remaining == seconds(5));
  CHECK(flags.has(stunned));

  // исчерпание: запись удаляется sweep-ом, has() гаснет ровно на границе.
  CHECK(flags.advance(seconds(5)) == 1);
  CHECK(flags.find(stunned) == nullptr);
  CHECK_FALSE(flags.has(stunned));
  CHECK(flags.has(blessed)); // насыщенное вычитание не убивает бессрочный

  // пауза: dt == 0 не двигает сроки.
  flags.set(stunned, seconds(3));
  CHECK(flags.advance({0}) == 0);
  CHECK(flags.find(stunned)->remaining == seconds(3));

  // нулевой остаток = мёртвый до sweep: has() уже false, advance(0) подчищает.
  flags.set(stunned, {0});
  CHECK_FALSE(flags.has(stunned));
  CHECK(flags.advance({0}) == 1);

  CHECK(flags.remove(blessed));
  CHECK_FALSE(flags.remove(blessed));
  CHECK(flags.empty());
}

TEST_CASE("flag_set entries are ordered by hash regardless of insertion order [aesthetics]") {
  const auto a = utils::string_hash("aaa");
  const auto b = utils::string_hash("bbb");
  const auto c = utils::string_hash("ccc");

  aesthetics::flag_set forward;
  forward.set(a);
  forward.set(b);
  forward.set(c);

  aesthetics::flag_set backward;
  backward.set(c);
  backward.set(b);
  backward.set(a);

  REQUIRE(forward.size() == backward.size());
  for (size_t i = 0; i < forward.size(); ++i) {
    CHECK(forward.entries[i].flag == backward.entries[i].flag);
  }
  for (size_t i = 1; i < forward.size(); ++i) {
    CHECK(forward.entries[i - 1].flag < forward.entries[i].flag);
  }
}
