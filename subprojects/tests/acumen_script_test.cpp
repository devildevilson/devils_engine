#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include <devils_script/system.h>
#include <devils_script/context.h>
#include <devils_script/container.h>
#include <tavl/parser.h>

#include "devils_engine/acumen/system.h"
#include "devils_engine/act/registry.h"
#include "devils_engine/act/function.h"
#include "devils_engine/act/exec_context.h"

// Обкатка co-parse GOAP-метрик (devils_script в tavl-потоке) + интеграция с acumen — контракт, на
// котором строится goap_resource. Метрика = "ключ = <ds-выражение>": tavl снимает ключ + '=', затем
// парсер отдаётся ds::system::parse, который доедает выражение до конца строки (row_end).
// Событийная последовательность (проверена дампом): array_begin, [row_begin, got_token(ключ),
// got_token('='), <токены выражения>, row_end], ..., array_end. ds::parse останавливается на row_end
// при нулевой вложенности (make_script_ast).

using namespace devils_engine;

namespace {

// Мок root-скоупа (без aesthetics/ECS — изолируем co-parse). Значение течёт через exec_context.w
// (reinterpret, как entity_scope в проекте); seed достаёт его в ds::context.
struct test_scope {
  double hunger = 0.0;
  double boredom = 0.0;
  bool valid() const noexcept { return true; }
};

double s_hunger(test_scope s)  { return s.hunger; }
double s_boredom(test_scope s) { return s.boredom; }

void seed_test_scope(const act::exec_context& ctx, devils_script::context* vm) {
  vm->set_arg(0, *reinterpret_cast<const test_scope*>(ctx.w));
}

devils_script::system make_sys() {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  sys.register_function<&s_hunger>("hunger");
  sys.register_function<&s_boredom>("boredom");
  return sys;
}

bool eval(const devils_script::container& c, const test_scope& s) {
  devils_script::context vm;
  vm.set_arg(0, s);
  c.process(&vm);
  return vm.get_return<bool>();
}

struct parsed_metric {
  std::string key;
  devils_script::container program;
};

// Ручной проезд tavl-парсера по массиву метрик: на каждый элемент снимаем ключ + '=', остаток строки
// доедает ds::system::parse. Ровно то, что делает goap_resource.
std::vector<parsed_metric> co_parse_metrics(const devils_script::system& sys, tavl::parser& p) {
  std::vector<parsed_metric> out;

  // до открытия массива
  for (;;) {
    const auto ev = p.peek();
    if (ev.type == tavl::event_type::array_begin) { p.poll_event(); break; }
    if (ev.type == tavl::event_type::eof || ev.type == tavl::event_type::not_enought_data) return out;
    p.poll_event();
  }

  // элементы до закрытия массива
  for (;;) {
    const auto ev = p.peek();
    if (ev.type == tavl::event_type::array_end || ev.type == tavl::event_type::eof) { p.poll_event(); break; }
    if (ev.type == tavl::event_type::row_begin || ev.type == tavl::event_type::row_end) { p.poll_event(); continue; }

    const auto [key_ev, key_err] = p.poll_event(); // got_token: ключ метрики
    parsed_metric m;
    m.key = p.to_string(key_ev.token);
    p.poll_event(); // got_token: '='
    devils_script::system::parse_context ctx;
    sys.parse<bool, test_scope>(m.key, p, ctx, m.program); // ds доедает выражение до row_end
    out.push_back(std::move(m));
  }
  return out;
}

} // namespace

TEST_CASE("ds parses one bool expression [devils_script]") {
  const auto sys = make_sys();
  tavl::parser p;
  sys.configure_parser(p);
  p.flush("hunger > 0.5");
  p.finish();

  devils_script::container c;
  devils_script::system::parse_context ctx;
  sys.parse<bool, test_scope>("is_hungry", p, ctx, c);

  CHECK(eval(c, test_scope{0.7, 0.0}) == true);
  CHECK(eval(c, test_scope{0.3, 0.0}) == false);
}

TEST_CASE("ds stops at row boundary — two expressions from one parser [devils_script]") {
  const auto sys = make_sys();
  tavl::parser p;
  sys.configure_parser(p);
  p.flush("hunger > 0.5\nboredom > 0.5");
  p.finish();

  devils_script::container c0, c1;
  devils_script::system::parse_context ctx0, ctx1;
  sys.parse<bool, test_scope>("is_hungry", p, ctx0, c0);
  sys.parse<bool, test_scope>("is_bored",  p, ctx1, c1);

  CHECK(eval(c0, test_scope{0.7, 0.0}) == true);
  CHECK(eval(c0, test_scope{0.3, 0.0}) == false);
  CHECK(eval(c1, test_scope{0.0, 0.7}) == true);
  CHECK(eval(c1, test_scope{0.0, 0.3}) == false);
}

TEST_CASE("co-parse: 'key = expr' metrics array [devils_script]") {
  const auto sys = make_sys();
  tavl::parser p;
  sys.configure_parser(p);
  p.flush("[ is_hungry = hunger > 0.5, is_bored = boredom > 0.5 ]");
  p.finish();

  const auto metrics = co_parse_metrics(sys, p);
  REQUIRE(metrics.size() == 2);
  CHECK(metrics[0].key == "is_hungry");
  CHECK(metrics[1].key == "is_bored");
  CHECK(eval(metrics[0].program, test_scope{0.7, 0.0}) == true);
  CHECK(eval(metrics[0].program, test_scope{0.3, 0.0}) == false);
  CHECK(eval(metrics[1].program, test_scope{0.0, 0.7}) == true);
}

TEST_CASE("co-parsed metrics drive acumen compute_state [acumen][devils_script]") {
  const auto sys = make_sys();
  tavl::parser p;
  sys.configure_parser(p);
  p.flush("[ is_hungry = hunger > 0.5, is_bored = boredom > 0.5 ]");
  p.finish();

  const auto parsed = co_parse_metrics(sys, p); // держим контейнеры живыми — script_function их заимствует
  REQUIRE(parsed.size() == 2);

  act::registry reg;
  std::vector<acumen::state_metric> metrics;
  for (const auto& m : parsed) {
    reg.reg(m.key, std::make_unique<act::script_function<bool>>(&m.program, &seed_test_scope));
    metrics.emplace_back(m.key); // порядок = индекс бита
  }

  // тривиальный goal, чтобы собрать систему (проверяем compute_state, не план).
  acumen::scoped_state goal_state; goal_state.set(0, true);
  std::vector<acumen::goal> goals = { acumen::goal{ "g", acumen::scoped_state{}, goal_state } };
  acumen::system goap(&reg, std::move(metrics), std::move(goals), std::vector<acumen::action>{});

  act::execution_scratch scratch;
  const auto run = [&](const test_scope& s) {
    act::exec_context ctx{};
    ctx.w = reinterpret_cast<const act::world*>(&s);
    ctx.scratch = &scratch;
    return goap.compute_state(ctx);
  };

  const auto s1 = run(test_scope{0.7, 0.3}); // hungry, not bored
  CHECK(s1.test(0) == true);
  CHECK(s1.test(1) == false);

  const auto s2 = run(test_scope{0.2, 0.9}); // not hungry, bored
  CHECK(s2.test(0) == false);
  CHECK(s2.test(1) == true);
}
