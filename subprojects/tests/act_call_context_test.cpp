#include <doctest/doctest.h>

#include <devils_script/context.h>
#include <devils_script/system.h>

#include <devils_engine/act/call_context.h>
#include <devils_engine/act/function.h>

using namespace devils_engine;

namespace {

act::real_t native_accumulate(const act::exec_context&, act::call_context& call) {
  auto& counter = call.argument("counter");
  counter = act::value::of(counter.kind == act::value_kind::integer ? counter.inum + 2 : int64_t(2));
  auto& values = call.list("values").values;
  values.push_back(act::value::of(int64_t(7)));
  return 9.0;
}

}

TEST_CASE("act call_context is shared with native functions [act]") {
  act::native_function<act::real_t> fn(&native_accumulate);
  act::exec_context exec;
  act::call_context call;
  call.reserve(2, 1);
  call.set("counter", act::value::of(int64_t(5)));
  call.list("values").values.reserve(4);

  CHECK(fn.invoke(exec, call) == doctest::Approx(9.0));
  REQUIRE(call.find_argument("counter") != nullptr);
  CHECK(call.find_argument("counter")->inum == 7);
  REQUIRE(call.find_list("values") != nullptr);
  REQUIRE(call.find_list("values")->values.size() == 1);
  CHECK(call.find_list("values")->values[0].inum == 7);
  CHECK(call.result.kind == act::value_kind::number);
  CHECK(call.result.num == doctest::Approx(9.0));
}

TEST_CASE("act call_context binds ds in-out arguments and lists [act][devils_script]") {
  devils_script::system sys;
  sys.init_basic_functions();
  sys.init_math();
  const auto program = sys.parse<double, void>(
    "accumulate",
    "{ ctx_set = { total = ctx:arg:total + 2.0 }, "
    "ctx:list:values = { add_to = 3.0, add_to = 4.0 }, ctx:arg:total }");

  devils_script::context vm;
  act::exec_context exec;
  exec.vm = &vm;
  act::call_context call;
  call.reserve(2, 1);
  call.set("total", act::value::of(5.0));
  call.list("values").values.reserve(4);

  act::script_function<act::real_t> fn(&program);
  CHECK(fn.invoke(exec, call) == doctest::Approx(7.0));
  REQUIRE(call.find_argument("total") != nullptr);
  CHECK(call.find_argument("total")->kind == act::value_kind::number);
  CHECK(call.find_argument("total")->num == doctest::Approx(7.0));
  REQUIRE(call.find_list("values") != nullptr);
  REQUIRE(call.find_list("values")->values.size() == 2);
  CHECK(call.find_list("values")->values[0].num == doctest::Approx(3.0));
  CHECK(call.find_list("values")->values[1].num == doctest::Approx(4.0));
}
