#include <cstdint>
#include <initializer_list>

#include <doctest/doctest.h>

#include <devils_engine/act/packer.h>

using devils_engine::act::call_context;
using devils_engine::act::category;
using devils_engine::act::entity_handle;
using devils_engine::act::entity_id;
using devils_engine::act::exec_context;
using devils_engine::act::pack;
using devils_engine::act::rng_source;
using devils_engine::act::value;

namespace {
struct captured {
  uint32_t a = 0, b = 0, c = 0;
  int dmg = 0;
  double k = 0.0;
  uint64_t tick = 0;
  bool called = false;
};
captured g;

// Тестовые геймплейные функции БЕЗ exec_context — упаковщик достаёт аргументы из контекста.
void t3(entity_handle a, entity_handle b, entity_handle c) {
  g = captured{};
  g.a = a.id.id;
  g.b = b.id.id;
  g.c = c.id.id;
  g.called = true;
}
void t2p(entity_handle a, entity_handle b, int dmg, double k) {
  g = captured{};
  g.a = a.id.id;
  g.b = b.id.id;
  g.dmg = dmg;
  g.k = k;
  g.called = true;
}
void trng(entity_handle a, rng_source rng) {
  g = captured{};
  g.a = a.id.id;
  g.tick = rng.tick;
  g.called = true;
}
bool tbool(entity_handle a) {
  return a.id.id > 5;
}
void tctx(const exec_context& ctx, entity_handle a) { // спец-случай: полный контекст
  g = captured{};
  g.a = a.id.id;
  g.tick = ctx.rng_tick;
  g.called = true;
}

exec_context make(const std::initializer_list<uint32_t> scope) {
  exec_context ctx;
  uint32_t i = 0;
  for (const auto s : scope) {
    ctx.scope[i++] = entity_id{s};
  }
  ctx.scope_count = uint32_t(scope.size());
  ctx.rng_seed = 1;
  ctx.rng_entity = 2;
  ctx.rng_tick = 42;
  ctx.w = nullptr; // тестовые функции world не дереференсят
  return ctx;
}
} // namespace

TEST_CASE("packer binds three entity handles from scope") {
  const auto f = pack<&t3>();
  auto ctx = make({10, 20, 30});
  call_context call;
  f->invoke(ctx, call);
  CHECK(g.called);
  CHECK(g.a == 10);
  CHECK(g.b == 20);
  CHECK(g.c == 30);
}

TEST_CASE("packer binds two handles plus plain args from call_context") {
  const auto f = pack<&t2p>();
  auto ctx = make({7, 8});
  call_context call;
  call.set("dmg", value::of(int64_t(15))); // plain[0]
  call.set("k", value::of(double(2.5)));    // plain[1]
  f->invoke(ctx, call);
  CHECK(g.a == 7);
  CHECK(g.b == 8);
  CHECK(g.dmg == 15);
  CHECK(g.k == doctest::Approx(2.5));
}

TEST_CASE("packer injects rng_source from context (least-privilege deterministic inputs)") {
  const auto f = pack<&trng>();
  auto ctx = make({3});
  call_context call;
  f->invoke(ctx, call);
  CHECK(g.a == 3);
  CHECK(g.tick == 42);
}

TEST_CASE("packer wraps a predicate (bool return) and reports category") {
  const auto f = pack<&tbool>();
  CHECK(f->cat == category::predicate);
  auto ctx1 = make({9});
  auto ctx2 = make({2});
  call_context call;
  CHECK(f->invoke(ctx1, call));       // 9 > 5
  CHECK_FALSE(f->invoke(ctx2, call)); // 2 > 5
}

TEST_CASE("packer passes full exec_context for special-case functions") {
  const auto f = pack<&tctx>();
  auto ctx = make({5});
  call_context call;
  f->invoke(ctx, call);
  CHECK(g.a == 5);
  CHECK(g.tick == 42); // достал tick из полного ctx
}
