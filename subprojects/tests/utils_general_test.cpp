#include <doctest/doctest.h>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/list.h"
#include "devils_engine/utils/block_allocator.h"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/mood/system.h"
#include "devils_engine/mood/registry.h"
#include "devils_engine/mood/runtime.h"
#include "devils_engine/act/registry.h"
#include "devils_engine/act/function.h"
#include "devils_engine/utils/kd_tree.h"
#include "devils_engine/utils/grid.h"
#include "devils_engine/utils/aabb_tree.h"
#include "devils_engine/utils/geometry.h"
#include "devils_engine/utils/type_traits.h" // template_string_concat / template_string_cat

#include <memory>
#include <vector>
#include <algorithm>

using namespace devils_engine;

enum list_values {
  list_type_1,
  list_type_2,
  list_type_3,
};

struct list_test_1_t : public utils::ring::list<list_test_1_t, list_type_1> {};
struct list_test_2_t : public utils::ring::list<list_test_2_t, list_type_2>, public utils::ring::list<list_test_2_t, list_type_3> {};

struct list_test4_t : public utils::forw::list<list_test4_t, list_type_1> {};

TEST_CASE("template_string compile-time concat [utils][template_string]") {
  using namespace devils_engine;
  // consteval конкат — проверяем в constant-expression (годность для NTTP).
  constexpr auto ab = utils::template_string_concat(utils::template_string_t("add_"), utils::template_string_t("strength"));
  static_assert(ab.sv() == std::string_view("add_strength"));
  static_assert(ab.size() == 12);

  constexpr auto three = utils::template_string_cat(
    utils::template_string_t("a"), utils::template_string_t("bc"), utils::template_string_t("def"));
  static_assert(three.sv() == std::string_view("abcdef"));
  CHECK(ab.sv() == "add_strength");
  CHECK(three.sv() == "abcdef");
}

TEST_CASE("Single and double linked lists [list]") {
  SUBCASE("ring list usage") {
    list_test_1_t lt1;
    list_test_1_t lt2;
    list_test_1_t lt3;
    list_test_1_t lt4;
    list_test_1_t lt5;

    // initially ring::list points to self, no nullptr are used
    auto lt1_p = static_cast<utils::ring::list<list_test_1_t, list_type_1>*>(&lt1); // DO NOT use casts, this one only for demonstration
    REQUIRE(lt1_p->m_next == &lt1);
    REQUIRE(lt1_p->m_prev == &lt1);
    // or we can say that list is empty (bad naming?)
    REQUIRE(lt1_p->empty());
    REQUIRE(utils::ring::list_empty<list_type_1>(&lt1));

    // there is 2 elements, they are pointing on each other
    utils::ring::list_add<list_type_1>(&lt1, &lt2); // lt2 added AFTER lt1
    REQUIRE(lt1_p->m_next == &lt2);
    REQUIRE(lt1_p->m_prev == &lt2);
    REQUIRE(!utils::ring::list_empty<list_type_1>(&lt1)); // not empty any more

    // there is 3 element: lt1 -> lt2 -> lt3 -> lt1
    utils::ring::list_add<list_type_1>(&lt2, &lt3); // lt3 added AFTER lt2
    REQUIRE(lt1_p->m_next == &lt2);
    REQUIRE(lt1_p->m_prev == &lt3);

    // there is 4 element: lt1 -> lt4 -> lt2 -> lt3 -> lt1
    utils::ring::list_add<list_type_1>(&lt1, &lt4); // lt4 added AFTER lt1
    REQUIRE(lt1_p->m_next == &lt4);
    REQUIRE(lt1_p->m_prev == &lt3);

    // there is 5 element: lt1 -> lt4 -> lt2 -> lt3 -> lt5 -> lt1
    utils::ring::list_radd<list_type_1>(&lt1, &lt5); // lt5 added BEFORE lt1
    REQUIRE(lt1_p->m_next == &lt4);
    REQUIRE(lt1_p->m_prev == &lt5);

    // to properly cycling thru list elements provide a reference for list_next
    size_t counter1 = 0;
    for (auto cur = &lt1; cur != nullptr; cur = utils::ring::list_next<list_type_1>(cur, &lt1)) {
      counter1 += 1;
    }

    size_t counter2 = 0;
    for (auto cur = &lt1; cur != nullptr; cur = utils::ring::list_prev<list_type_1>(cur, &lt1)) {
      counter2 += 1;
    }

    REQUIRE(counter1 == 5);
    REQUIRE(counter2 == 5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt1) == 5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt2) == 5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt3) == 5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt4) == 5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt5) == 5);

    {
      list_test_1_t lt6;

      utils::ring::list_radd<list_type_1>(&lt1, &lt6); // lt5 added BEFORE lt1
      REQUIRE(utils::ring::list_count<list_type_1>(&lt1) == 6);
      // when lt6 goes out of scope it triggers remove from list
    }

    REQUIRE(utils::ring::list_count<list_type_1>(&lt1) == 5);

    utils::ring::list_remove<list_type_1>(&lt5);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt1) == 4);

    // unfortunately there is no way to put any of list_test_1_t in two lists at the same time
    list_test_1_t lt6;
    utils::ring::list_radd<list_type_1>(&lt6, &lt4); // lt4 added BEFORE lt6
    REQUIRE(utils::ring::list_count<list_type_1>(&lt1) == 3);
    REQUIRE(utils::ring::list_count<list_type_1>(&lt6) == 2);

    // use lists with defferent types

    list_test_2_t lt21;
    list_test_2_t lt22;
    list_test_2_t lt23;

    utils::ring::list_radd<list_type_2>(&lt21, &lt22); // note ! list_type_2
    utils::ring::list_radd<list_type_3>(&lt23, &lt22); // note ! list_type_3
    REQUIRE(utils::ring::list_count<list_type_2>(&lt21) == 2);
    REQUIRE(utils::ring::list_count<list_type_3>(&lt23) == 2);

    // ring list api additionally provides way to break list completely
    // usually you dont need it
    utils::ring::list_invalidate<list_type_1>(&lt3);
    REQUIRE(utils::ring::list_empty<list_type_1>(&lt3)); // lt3 empty
    REQUIRE(lt1_p->m_prev == &lt3);                      // but lt1 points to lt3
    // this breaks ciclyng and makes list_count executes forever
  }

  SUBCASE("forw list usage") {
    list_test4_t lt1;
    list_test4_t lt2;
    list_test4_t lt3;

    // as usual added AFTER first arg
    utils::forw::list_add<list_type_1>(&lt1, &lt2); // lt1 -> lt2
    utils::forw::list_add<list_type_1>(&lt1, &lt3); // lt1 -> lt3 -> lt2
    REQUIRE(utils::forw::list_count<list_type_1>(&lt1) == 3);
    REQUIRE(utils::forw::list_empty<list_type_1>(&lt2));
    REQUIRE(!utils::forw::list_empty<list_type_1>(&lt3));

    // unfortunatelly removing is harder
    utils::forw::list_remove<list_type_1>(&lt1, &lt2); // lt1 -> lt3
    utils::forw::list_remove<list_type_1>(&lt1, &lt3); // lt1
    REQUIRE(utils::forw::list_empty<list_type_1>(&lt1));
    REQUIRE(utils::forw::list_empty<list_type_1>(&lt2));
    REQUIRE(utils::forw::list_empty<list_type_1>(&lt3));

    {
      list_test4_t lt4;
      utils::forw::list_add<list_type_1>(&lt1, &lt4);
      // NO AUTO REMOVING AFTER SCOPE!!!
    }

    REQUIRE(!utils::forw::list_empty<list_type_1>(&lt1));
    utils::forw::list_invalidate<list_type_1>(&lt1);
  }
}

struct basic_struct {
  int a;

  basic_struct(int a) noexcept : a(a) {}
  ~basic_struct() noexcept { a = 0; }
};

TEST_CASE("Block allocator tests [block_allocator]") {
  const size_t overall_size = 4 * 1024 * 1024; // 4KB
  const size_t block_size = 256;
  const size_t alignment = 16;
  utils::block_allocator al(overall_size, block_size, alignment);

  // utility
  REQUIRE(al.alignment() == alignment);
  REQUIRE(al.allocation_size() == utils::align_to(block_size, alignment));
  REQUIRE(al.size() == utils::align_to(overall_size, alignment));
  REQUIRE(al.compute_full_size() == 0); // lazy allocation

  {
    auto ptr = al.allocate();
    REQUIRE(ptr != nullptr);

    al.free(ptr);
  }

  REQUIRE(al.compute_full_size() == al.size());

  {
    auto ptr = al.create<basic_struct>(4); // placement new
    REQUIRE(ptr->a == 4);
    al.destroy(ptr);
    // dont use pointers after they being destructed
  }
}

TEST_CASE("Mood system tests [mood::system]") {
  const std::initializer_list<std::string_view> a_strs = { "action1", "action2", "action3", "action4", "action5" };
  const std::initializer_list<std::string_view> g_strs = { "guard1", "guard2", "guard3", "guard4", "guard5", "guard6", };

  // общий реестр геймплейных функций (бывшая mood::table): action = effect (void),
  // guard = predicate (bool). Имена резолвятся mood::system по string_hash.
  act::registry t;
  for (const auto& name : a_strs) {
    t.reg(name, std::make_unique<act::native_function<void>>(+[] (const act::exec_context&) {}));
  }

  for (const auto& name : g_strs) {
    t.reg(name, std::make_unique<act::native_function<bool>>(+[] (const act::exec_context&) { return true; }));
  }

  const std::vector<std::string> lines = {
    "begin + idle = prepare_weapon",
    "initial_state [guard1] = prepare_weapon1",
    "initial_state = prepare_weapon",
    "prepare_weapon + idle = melee_ready",
    "melee_ready + attack = melee_attack",
    "melee_attack + idle = melee_attack_end",
    "melee_attack + on_entry / action2, action3, action4",
    "melee_attack + on_exit / action2",
    "melee_attack_end + idle = melee_ready",
    "melee_attack_end + attack [guard1] / action1 = melee_attack2",
    "melee_attack_end + attack [guard2] / action2 = melee_attack2",
    "melee_attack_end + attack [guard5] / action5 = melee_attack2",
    "melee_attack_end + attack [guard3] / action3 = melee_attack2",
    "melee_attack_end + fdfgwewg [ guard1, guard2, guard3 ] / action1 = melee_attack2",
    "any_state + attack [guard1] / action1 = melee_attack2",
  };

  mood::system s(&t, lines);
  mood::registry fsms;
  const auto fsm_id = fsms.add("combat", mood::system(&t, lines));
  REQUIRE(fsms.get(fsm_id) != nullptr);
  REQUIRE(fsms.get("combat")->transitions().size() == s.transitions().size());

  const auto trans1 = s.find_transitions("begin", "idle"); // find transition from state 'begin' thru event 'idle'
  REQUIRE(trans1.size() == 1); // 1 transition
  REQUIRE(trans1[0].current_state == "begin");
  REQUIRE(trans1[0].event == "idle");
  REQUIRE(trans1[0].next_state == "prepare_weapon");
  const auto trans2 = s.find_transitions("begin", "attack");
  REQUIRE(trans2.size() == 0); // no such transitions
  const auto trans3 = s.find_transitions("melee_attack_end", "attack");
  REQUIRE(trans3.size() == 4); // 4 transitions
  REQUIRE(trans3[0].current_state == "melee_attack_end");
  REQUIRE(trans3[0].event == "attack");
  REQUIRE(trans3[0].next_state == "melee_attack2");
  REQUIRE(trans3[0].guards[0] == "guard1");
  REQUIRE(trans3[1].guards[0] == "guard2");
  REQUIRE(trans3[2].guards[0] == "guard5");
  REQUIRE(trans3[3].guards[0] == "guard3");
  REQUIRE(trans3[0].actions[0] == "action1");
  REQUIRE(trans3[1].actions[0] == "action2");
  REQUIRE(trans3[2].actions[0] == "action5");
  REQUIRE(trans3[3].actions[0] == "action3");
  const auto trans4 = s.find_transitions("initial_state", "");
  REQUIRE(trans4.size() == 2); // 2 transitions
  REQUIRE(trans4[0].event == "");
  REQUIRE(trans4[0].guards[0] == "guard1");
  REQUIRE(trans4[1].guards[0] == ""); // no guards at all
  const auto trans5 = s.find_transitions("melee_attack", "idle");
  REQUIRE(trans5.size() == 1);
  // on_exit/on_entry — это конвенции helper-слоя, system их не понимает: ищем напрямую как
  // обычные псевдо-события (раньше это кэшировалось в transition::current_state_on_exit/...).
  REQUIRE(s.find_transitions(trans5[0].current_state, "on_exit").size() == 1); // melee_attack + on_exit
  REQUIRE(s.find_transitions(trans5[0].next_state, "on_entry").size() == 0);   // melee_attack_end + on_entry
  const auto trans6 = s.find_transitions("melee_ready", "attack");
  REQUIRE(trans6.size() == 1);
  REQUIRE(trans6[0].next_state == "melee_attack");
  REQUIRE(s.find_transitions(trans6[0].current_state, "on_exit").size() == 0); // melee_ready + on_exit
  REQUIRE(s.find_transitions(trans6[0].next_state, "on_entry").size() == 1);   // melee_attack + on_entry

  // доступ по предпосчитанному хешу даёт тот же результат, что и по строке.
  REQUIRE(s.find_transitions(utils::string_hash("melee_attack_end"), utils::string_hash("attack")).size() == 4);

  // helper-слой: шаг автомата. Все зарегистрированные гварды возвращают true.
  act::execution_scratch execution;
  act::exec_context ctx{}; // sink == nullptr => dry-run; гварды ctx не используют
  ctx.scratch = &execution;
  const auto st1 = mood::step(s, "melee_ready", "attack", ctx);
  REQUIRE(st1.result == mood::step_result::transitioned);
  REQUIRE(st1.next_state == utils::string_hash("melee_attack"));
  // apply_transition: прогоняет on_exit(melee_ready)=пусто → эффекты перехода → on_entry(melee_attack)
  // = action2/3/4 (no-op в тесте), возвращает хеш нового состояния. Не бросает.
  REQUIRE(mood::apply_transition(s, utils::string_hash("melee_ready"), *st1.taken, ctx) == utils::string_hash("melee_attack"));
  // fallback на any_state: у 'begin' нет своего 'attack' (trans2 пуст), но any_state + attack есть.
  REQUIRE(s.find_transitions("begin", "attack").size() == 0);
  const auto st2 = mood::step(s, "begin", "attack", ctx);
  REQUIRE(st2.result == mood::step_result::transitioned); // подхватился any_state
  REQUIRE(st2.next_state == utils::string_hash("melee_attack2"));
  // нет перехода вообще (даже через any_state) — скорее опечатка в имени события.
  const auto st3 = mood::step(s, "begin", "no_such_event", ctx);
  REQUIRE(st3.result == mood::step_result::no_transition);
  REQUIRE(st3.candidates == 0);

  mood::validate(s); // не падает; выкидывает warning'и по тупиковым/недостижимым (any_state и т.п.)
}

static int g_mood_mark_calls = 0;

TEST_CASE("Mood runtime: blocked / internal / settle / limits [mood]") {
  g_mood_mark_calls = 0;
  act::registry t;
  t.reg("never",  std::make_unique<act::native_function<bool>>(+[](const act::exec_context&){ return false; }));
  t.reg("always", std::make_unique<act::native_function<bool>>(+[](const act::exec_context&){ return true; }));
  t.reg("mark",   std::make_unique<act::native_function<void>>(+[](const act::exec_context&){ ++g_mood_mark_calls; }));

  const std::vector<std::string> lines = {
    "s0 + go [never] = s1",   // гвард всегда false → blocked
    "s0 + tick / mark",       // внутренний переход (нет '= next'): эффект, состояние не меняется
    "a + idle = b",           // цепочка completion-переходов для settle
    "b + idle = c",
    "c + idle = c",           // само-петля → settle обязан остановиться
    "x + idle = y",           // цикл x<->y → settle останавливается по капу
    "y + idle = x",
  };
  mood::system s(&t, lines);
  act::execution_scratch execution;
  act::exec_context ctx{}; // dry-run ctx; эффекты в тесте no-op/счётчик
  ctx.scratch = &execution;

  // blocked: переход по (s0, go) ЕСТЬ, но гвард 'never' его отсеял
  const auto b = mood::step(s, "s0", "go", ctx);
  REQUIRE(b.result == mood::step_result::blocked);
  REQUIRE(b.candidates == 1);

  // internal: transitioned, но next пуст → apply возвращает invalid_id (состояние не меняем), эффект сработал
  const auto in = mood::step(s, "s0", "tick", ctx);
  REQUIRE(in.result == mood::step_result::transitioned);
  REQUIRE(in.taken != nullptr);
  REQUIRE(in.taken->next_hash == utils::invalid_id);
  REQUIRE(mood::apply_transition(s, utils::string_hash("s0"), *in.taken, ctx) == utils::invalid_id);
  REQUIRE(g_mood_mark_calls == 1);

  // settle: a → b → c (idle completion-цепочка), затем c+idle=c само-петля → стоп на 'c'
  REQUIRE(mood::settle(s, utils::string_hash("a"), mood::conv::idle, ctx) == utils::string_hash("c"));

  // settle с циклом x<->y обязан ТЕРМИНИРОВАТЬ по капу (не зациклиться), вернув x или y
  const auto cyc = mood::settle(s, utils::string_hash("x"), mood::conv::idle, ctx, 8);
  REQUIRE((cyc == utils::string_hash("x") || cyc == utils::string_hash("y")));

  // лимит гвардов (>8) и неизвестный символ → ошибка на парсинге в конструкторе
  REQUIRE_THROWS(mood::system(&t, std::vector<std::string>{ "st + ev [g1,g2,g3,g4,g5,g6,g7,g8,g9] = q" }));
  REQUIRE_THROWS(mood::system(&t, std::vector<std::string>{ "st + @ = q" }));
}

TEST_CASE("String utility tests [utils::string]") {
  SUBCASE("split test normal usage 'abc.ab.rtetr.bac.wert'") {
    const std::string_view test1 = "abc.ab.rtetr.bac.wert";

    std::array<std::string_view, 3> arr1;
    auto ans1 = std::span(arr1.data(), arr1.size());
    const size_t ret1 = utils::string::split(test1, ".", ans1); // can be used with span
    REQUIRE(ret1 == SIZE_MAX); // arr size too small for this string
    REQUIRE(arr1[0] == "abc");
    REQUIRE(arr1[1] == "ab");
    REQUIRE(arr1[2] == "rtetr.bac.wert"); // last one holds remainder of original string

    std::array<std::string_view, 6> arr2;
    const size_t ret2 = utils::string::split(test1, ".", arr2.data(), arr2.size()); // can be used without span
    REQUIRE(ret2 == 5); // arr size is ok fow this string
    REQUIRE(arr2[0] == "abc");
    REQUIRE(arr2[1] == "ab");
    REQUIRE(arr2[2] == "rtetr");
    REQUIRE(arr2[3] == "bac");
    REQUIRE(arr2[4] == "wert");
    REQUIRE(arr2[5] == ""); // no data
  }

  SUBCASE("split test abnormal usage 'qwertyuiop', 'asd.....asd.....asd', ''") {
    const std::string_view test1 = "qwertyuiop";
    const std::string_view test2 = "asd.....asd.....asd";
    const std::string_view test3 = "";

    std::array<std::string_view, 10> arr;
    auto ans = std::span(arr.data(), arr.size());
    size_t ret = 0;

    ret = utils::string::split(test1, ".", ans);
    REQUIRE(ret == 1);
    REQUIRE(arr[0] == "qwertyuiop");
    REQUIRE(arr[1] == "");

    ret = utils::string::split(test2, ".", ans);
    REQUIRE(ret == SIZE_MAX);
    REQUIRE(arr[0] == "asd");
    REQUIRE(arr[1] == "");
    REQUIRE(arr[2] == "");
    REQUIRE(arr[3] == "");
    REQUIRE(arr[4] == "");
    REQUIRE(arr[5] == "asd");
    REQUIRE(arr[9] == ".asd");

    ret = utils::string::split(test3, ".", ans);
    REQUIRE(ret == 0);
  }

  SUBCASE("split test different token usage") {
    const std::string_view test1 = "qwertyuiop";
    const std::string_view test2 = "asd.....asd.....asd";
    const std::string_view test3 = "ababababbbabbabababaabaababababbabababaaaababb";

    std::array<std::string_view, 100> arr;
    auto ans = std::span(arr.data(), arr.size());
    size_t ret = 0;

    ret = utils::string::split(test1, "yu", ans);
    REQUIRE(ret == 2);
    REQUIRE(arr[0] == "qwert");
    REQUIRE(arr[1] == "iop");

    ret = utils::string::split(test2, ".....", ans);
    REQUIRE(ret == 3);
    REQUIRE(arr[0] == "asd");
    REQUIRE(arr[1] == "asd");
    REQUIRE(arr[2] == "asd");

    ret = utils::string::split(test3, "bab", ans);
    REQUIRE(ret == 11);
    REQUIRE(arr[0]  == "a");
    REQUIRE(arr[1]  == "a");
    REQUIRE(arr[2]  == "b");
    REQUIRE(arr[3]  == "");
    REQUIRE(arr[4]  == "a");
    REQUIRE(arr[5]  == "aabaa");
    REQUIRE(arr[6]  == "a");
    REQUIRE(arr[7]  == "");
    REQUIRE(arr[8]  == "a");
    REQUIRE(arr[9]  == "aaaa");
    REQUIRE(arr[10] == "b");
  }

  SUBCASE("inside2 test") {
    const std::string_view test1 = "{ \"abbbaab\": \"abc\", \"babav\": [ \"ababab\", 123, { \"bfsdb\": [ \"abc\" ] } ] }";
    const auto ret = utils::string::trim(utils::string::inside2(test1, "[", "]"));
    REQUIRE(ret == "\"ababab\", 123, { \"bfsdb\": [ \"abc\" ] }");
  }

  SUBCASE("trim test") {
    REQUIRE(utils::string::trim("   a    ") == "a");
    REQUIRE(utils::string::trim("   a") == "a");
    REQUIRE(utils::string::trim("a    ") == "a");
    REQUIRE(utils::string::trim("a") == "a");
    REQUIRE(utils::string::trim("\n\t\v\ra    ") == "a");
  }

  SUBCASE("stoi test") {
    REQUIRE(utils::string::stoi("10") == 10);
    REQUIRE(utils::string::stoi("34634634") == 34634634);
    REQUIRE(utils::string::stoi("1") == 1);
    REQUIRE(utils::string::stoi("") == 0);
    REQUIRE(utils::string::stoi("a1") == 0);
    REQUIRE(utils::string::stoi("-1") == 0);
  }

  SUBCASE("slice test") {
    REQUIRE(utils::string::slice("qwertyuiop") == "qwertyuiop");
    REQUIRE(utils::string::slice("qwertyuiop", 1) == "wertyuiop");
    REQUIRE(utils::string::slice("qwertyuiop", 1, 1) == "");
    REQUIRE(utils::string::slice("qwertyuiop", 1, 2) == "w");
    REQUIRE(utils::string::slice("qwertyuiop", -5, -2) == "yui");
    REQUIRE(utils::string::slice("qwertyuiop", 3, -3) == "rtyu");
  }

  SUBCASE("find_ci test") {
    REQUIRE(utils::string::find_ci("QWERTYUIOP", "qwe") == 0);
    REQUIRE(utils::string::find_ci("QwErTyUiOp", "qWe") == 0);
    REQUIRE(utils::string::find_ci("QwErTyUiOp", "ert") == 2);
  }
}

TEST_CASE("kd_tree nearest/radius with predicate [utils::kd_tree]") {
  struct pl { uint32_t id; float size; };
  struct pt { float x, y; uint32_t id; float size; };
  const std::vector<pt> pts = {
    {  0.0f,  0.0f, 0, 1.0f }, {  1.0f,  0.0f, 1, 2.0f }, {  0.0f,  1.0f, 2, 0.5f },
    {  3.0f,  3.0f, 3, 3.0f }, { -2.0f,  1.0f, 4, 2.5f }, {  5.0f,  5.0f, 5, 0.2f },
    {  2.0f, -1.0f, 6, 1.5f }, { -1.0f, -1.0f, 7, 4.0f }, {  4.0f,  0.0f, 8, 0.8f },
    {  0.0f,  4.0f, 9, 2.2f },
  };

  utils::kd_tree<pl> tree;
  for (const auto& p : pts) tree.insert(utils::kd_tree<pl>::point{ p.x, p.y }, pl{ p.id, p.size });
  tree.build();
  REQUIRE(tree.size() == pts.size());

  // брутфорс: минимальная дистанция² среди СТРОГО крупнее (исключая self) в радиусе r.
  const auto brute_d2 = [&](const pt& self, const float r) {
    float best = r * r;
    for (const auto& p : pts) {
      if (p.id == self.id || !(p.size > self.size)) continue;
      const float dx = p.x - self.x, dy = p.y - self.y, d2 = dx * dx + dy * dy;
      if (d2 < best) best = d2;
    }
    return best; // == r*r если никого нет
  };

  SUBCASE("nearest-bigger matches brute force for every actor") {
    const float r = 100.0f;
    for (const auto& self : pts) {
      const auto* n = tree.nearest(utils::kd_tree<pl>::point{ self.x, self.y }, r,
        [&](const pl& p) { return p.id != self.id && p.size > self.size; });
      const float bd2 = brute_d2(self, r);
      if (bd2 >= r * r) {
        REQUIRE(n == nullptr); // никого крупнее
      } else {
        REQUIRE(n != nullptr);
        const float dx = n->pos[0] - self.x, dy = n->pos[1] - self.y;
        REQUIRE((dx * dx + dy * dy) == doctest::Approx(bd2)); // та же дистанция (устойчиво к ничьим)
        REQUIRE(n->payload.size > self.size);
      }
    }
  }

  SUBCASE("radius bound excludes targets beyond it") {
    // у точки 5 (5,5, size 0.2) все крупнее далеко — крошечный радиус ⇒ никого.
    const auto* n = tree.nearest(utils::kd_tree<pl>::point{ 5.0f, 5.0f }, 0.5f,
      [](const pl& p) { return p.size > 0.2f; });
    REQUIRE(n == nullptr);
  }

  SUBCASE("predicate excludes self") {
    const auto* n = tree.nearest(utils::kd_tree<pl>::point{ 0.0f, 0.0f }, 100.0f,
      [](const pl& p) { return p.id != 0; });
    REQUIRE(n != nullptr);
    REQUIRE(n->payload.id != 0u);
  }

  SUBCASE("empty tree returns nullptr") {
    utils::kd_tree<pl> empty;
    empty.build();
    REQUIRE(empty.nearest(utils::kd_tree<pl>::point{ 0.0f, 0.0f }, 10.0f,
      [](const pl&) { return true; }) == nullptr);
  }

  SUBCASE("radius visits exactly the in-range set") {
    std::vector<uint32_t> got;
    tree.radius(utils::kd_tree<pl>::point{ 0.0f, 0.0f }, 1.5f, [](const pl&) { return true; },
      [&](const utils::kd_tree<pl>::node& n) { got.push_back(n.payload.id); });
    std::vector<uint32_t> exp;
    for (const auto& p : pts) if (p.x * p.x + p.y * p.y <= 1.5f * 1.5f) exp.push_back(p.id);
    std::sort(got.begin(), got.end());
    std::sort(exp.begin(), exp.end());
    REQUIRE(got == exp);
  }

  SUBCASE("nearest2 equals two separate nearest calls") {
    const auto d2 = [](const utils::kd_tree<pl>::node* n, const pt& self) {
      if (n == nullptr) return -1.0f;
      const float dx = n->pos[0] - self.x, dy = n->pos[1] - self.y;
      return dx * dx + dy * dy;
    };
    const float r = 100.0f;
    for (const auto& self : pts) {
      const utils::kd_tree<pl>::point q{ self.x, self.y };
      const auto bigger  = [&](const pl& p) { return p.id != self.id && p.size > self.size; };
      const auto smaller = [&](const pl& p) { return p.id != self.id && p.size < self.size; };
      const auto [a2, b2] = tree.nearest2(q, r, bigger, smaller);
      const auto* a1 = tree.nearest(q, r, bigger);
      const auto* b1 = tree.nearest(q, r, smaller);
      REQUIRE((a2 == nullptr) == (a1 == nullptr));
      REQUIRE((b2 == nullptr) == (b1 == nullptr));
      if (a1 != nullptr) REQUIRE(d2(a2, self) == doctest::Approx(d2(a1, self)));
      if (b1 != nullptr) REQUIRE(d2(b2, self) == doctest::Approx(d2(b1, self)));
    }
  }
}

TEST_CASE("geometry primitive predicates [utils::geom]") {
  using namespace utils::geom;
  using v2 = std::array<float, 2>;
  using v3 = std::array<float, 3>;

  SUBCASE("aabb / sphere basics 2D") {
    aabb<v2, 2> b{ { 0, 0 }, { 10, 10 } };
    REQUIRE(contains(b, v2{ 5, 5 }));
    REQUIRE_FALSE(contains(b, v2{ 11, 5 }));
    sphere<v2, 2> s{ { 5, 5 }, 2.0f };
    REQUIRE(contains(s, v2{ 6, 6 }));            // dist √2 < 2
    REQUIRE_FALSE(contains(s, v2{ 8, 8 }));      // dist √18 > 2
    REQUIRE(overlaps(s, b));
    REQUIRE_FALSE(overlaps(sphere<v2, 2>{ { 100, 100 }, 1.0f }, b));
  }

  SUBCASE("ray slab test respects tmax") {
    aabb<v2, 2> b{ { 5, -1 }, { 6, 1 } };
    REQUIRE(overlaps(ray<v2, 2>{ { 0, 0 }, { 1, 0 }, 100.0f }, b));  // достаёт
    REQUIRE_FALSE(overlaps(ray<v2, 2>{ { 0, 0 }, { 1, 0 }, 3.0f }, b)); // tmax коротко
    REQUIRE_FALSE(overlaps(ray<v2, 2>{ { 0, 0 }, { 0, 1 }, 100.0f }, b)); // мимо (вверх)
  }

  SUBCASE("cylinder flat caps 2D") {
    cylinder<v2, 2> c{ { 0, 0 }, { 1, 0 }, 10.0f, 1.0f };
    REQUIRE(contains(c, v2{ 5, 0.5f }));   // в пределах длины и радиуса
    REQUIRE_FALSE(contains(c, v2{ 5, 2 }));  // за радиусом
    REQUIRE_FALSE(contains(c, v2{ 11, 0 })); // за торцом (плоский торец)
    REQUIRE_FALSE(contains(c, v2{ -0.5f, 0 })); // до начала
  }

  SUBCASE("up_cylinder ignores UP axis (Y) 3D") {
    up_cylinder<v3, 3> uc{ { 0, 0, 0 }, 1.0f };
    REQUIRE(contains<1>(uc, v3{ 0.5f, 9999, 0.5f }));   // высоко по Y — всё равно внутри колонны
    REQUIRE_FALSE(contains<1>(uc, v3{ 2, 0, 0 }));      // вне радиуса в плоскости XZ
    aabb<v3, 3> far{ { 5, 0, 5 }, { 6, 1, 6 } };
    REQUIRE_FALSE(overlaps<1>(uc, far));
  }

  SUBCASE("obb vs aabb SAT 2D — rotated box separated by a gap") {
    std::array<v2, 2> ax{ v2{ 0.7071f, 0.7071f }, v2{ -0.7071f, 0.7071f } }; // 45°
    obb<v2, 2> o{ { 0, 0 }, { 1, 1 }, ax };
    REQUIRE(overlaps(o, aabb<v2, 2>{ { -0.5f, -0.5f }, { 0.5f, 0.5f } }));
    REQUIRE_FALSE(overlaps(o, aabb<v2, 2>{ { 3, 3 }, { 4, 4 } }));
    REQUIRE(contains(o, v2{ 0, 1 }));          // вдоль повёрнутой оси
    REQUIRE_FALSE(contains(o, v2{ 1.5f, 0 }));
  }
}

TEST_CASE("spatial query API: point stores match brute force [utils::kd_tree,dense_grid,hash_grid]") {
  using v2 = std::array<float, 2>;
  struct pl { uint32_t id; };
  std::vector<v2> pts;
  for (int i = 0; i < 60; ++i) // детерминированное «облако» (без rand)
    pts.push_back(v2{ float((i * 37) % 41) * 0.5f - 10.0f, float((i * 53) % 43) * 0.5f - 10.0f });

  utils::kd_tree<pl, v2, 2> kd;
  utils::hash_grid<pl, v2, 2> hg(2.0f);
  utils::dense_grid<pl, v2, 2> dg(v2{ -12, -12 }, { 24, 24 }, 1.0f); // покрывает [-12,12)
  for (int i = 0; i < int(pts.size()); ++i) {
    kd.insert(pts[i], pl{ uint32_t(i) });
    hg.insert(pts[i], pl{ uint32_t(i) });
    dg.insert(pts[i], pl{ uint32_t(i) });
  }
  kd.build();

  const auto check = [&](auto shape) {
    std::vector<uint32_t> brute, a, b, c;
    for (int i = 0; i < int(pts.size()); ++i)
      if (utils::geom::test_contains<1>(shape, pts[i])) brute.push_back(uint32_t(i));
    kd.query(shape, [&](const pl& p) { a.push_back(p.id); });
    hg.query(shape, [&](const pl& p) { b.push_back(p.id); });
    dg.query(shape, [&](const pl& p) { c.push_back(p.id); });
    std::sort(brute.begin(), brute.end());
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end()); std::sort(c.begin(), c.end());
    REQUIRE(a == brute);
    REQUIRE(b == brute);
    REQUIRE(c == brute);
  };

  check(utils::geom::aabb<v2, 2>{ { -3, -3 }, { 4, 5 } });
  check(utils::geom::sphere<v2, 2>{ { 0, 0 }, 3.5f });
  check(utils::geom::cylinder<v2, 2>{ { -8, -8 }, { 0.7071f, 0.7071f }, 25.0f, 1.5f });
  check(utils::geom::obb<v2, 2>{ { 1, 1 }, { 3, 1 }, { v2{ 0.7071f, 0.7071f }, v2{ -0.7071f, 0.7071f } } });

  SUBCASE("grids clear() then reuse") {
    hg.clear(); dg.clear();
    REQUIRE(hg.empty()); REQUIRE(dg.empty());
    hg.insert(v2{ 0, 0 }, pl{ 99 }); dg.insert(v2{ 0, 0 }, pl{ 99 });
    int nh = 0, nd = 0;
    hg.query(utils::geom::sphere<v2, 2>{ { 0, 0 }, 1.0f }, [&](const pl& p) { REQUIRE(p.id == 99u); ++nh; });
    dg.query(utils::geom::sphere<v2, 2>{ { 0, 0 }, 1.0f }, [&](const pl& p) { REQUIRE(p.id == 99u); ++nd; });
    REQUIRE(nh == 1); REQUIRE(nd == 1);
  }
}

TEST_CASE("spatial grids: incremental add/remove/update [utils::dense_grid,hash_grid]") {
  using v2 = std::array<float, 2>;
  struct pl { uint32_t id; };
  utils::hash_grid<pl, v2, 2> hg(2.0f);
  utils::dense_grid<pl, v2, 2> dg(v2{ -12, -12 }, { 24, 24 }, 1.0f);
  std::vector<std::pair<uint32_t, v2>> ref;                 // живой эталон
  std::vector<utils::grid_handle> hh(60), dh(60);

  const auto pt = [](int i) { return v2{ float((i * 37) % 41) * 0.5f - 10.0f, float((i * 53) % 43) * 0.5f - 10.0f }; };
  const auto check = [&](auto shape) {
    std::vector<uint32_t> brute, a, b;
    for (auto& [id, p] : ref) if (utils::geom::test_contains<1>(shape, p)) brute.push_back(id);
    hg.query(shape, [&](const pl& p) { a.push_back(p.id); });
    dg.query(shape, [&](const pl& p) { b.push_back(p.id); });
    std::sort(brute.begin(), brute.end()); std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    REQUIRE(a == brute);
    REQUIRE(b == brute);
  };
  const auto probe = [&] {
    check(utils::geom::sphere<v2, 2>{ { 0, 0 }, 4.0f });
    check(utils::geom::aabb<v2, 2>{ { -5, -5 }, { 6, 6 } });
  };

  for (int i = 0; i < 60; ++i) { const v2 p = pt(i); hh[i] = hg.insert(p, pl{ uint32_t(i) }); dh[i] = dg.insert(p, pl{ uint32_t(i) }); ref.push_back({ uint32_t(i), p }); }
  probe();

  for (int i = 0; i < 60; i += 3) { hg.remove(hh[i]); dg.remove(dh[i]); } // удалить каждый 3-й
  ref.erase(std::remove_if(ref.begin(), ref.end(), [](auto& e) { return e.first % 3 == 0; }), ref.end());
  REQUIRE(hg.size() == ref.size());
  REQUIRE(dg.size() == ref.size());
  probe();

  for (int i = 0; i < 60; ++i) { if (i % 3 == 0) continue; const v2 np{ float((i * 29) % 37) * 0.5f - 9.0f, float((i * 17) % 31) * 0.5f - 7.0f };
    hg.update(hh[i], np); dg.update(dh[i], np); for (auto& e : ref) if (e.first == uint32_t(i)) e.second = np; }
  probe();

  SUBCASE("stale handle remove/update is a safe no-op") {
    const size_t before = hg.size();
    hg.remove(hh[0]); dg.remove(dh[0]);   // hh[0] уже удалён на шаге remove
    hg.update(hh[0], v2{ 0, 0 });
    REQUIRE(hg.size() == before);
    REQUIRE(dg.size() == before);
  }
}

TEST_CASE("spatial query API: aabb_tree (dynamic BVH) matches brute force [utils::aabb_tree]") {
  using v2 = std::array<float, 2>;
  using box2 = utils::geom::aabb<v2, 2>;
  struct pl { uint32_t id; };
  const auto mk = [](int i, float ox = 0, float oy = 0) { const float x = float((i * 7) % 37) + ox, y = float((i * 13) % 29) + oy; return box2{ { x, y }, { x + 2.0f, y + 1.5f } }; };

  utils::aabb_tree<pl, v2, 2> tree;
  std::vector<std::pair<uint32_t, box2>> ref;
  std::vector<utils::bvh_handle> h(80);
  for (int i = 0; i < 60; ++i) { const box2 b = mk(i); h[i] = tree.insert(b, pl{ uint32_t(i) }); ref.push_back({ uint32_t(i), b }); }
  REQUIRE(tree.size() == 60);

  const auto check = [&](auto shape) {
    std::vector<uint32_t> brute, got;
    for (auto& [id, b] : ref) if (utils::geom::test_overlaps<1>(shape, b)) brute.push_back(id);
    tree.query(shape, [&](const pl& p) { got.push_back(p.id); });
    std::sort(brute.begin(), brute.end()); std::sort(got.begin(), got.end());
    REQUIRE(got == brute);
  };
  const auto probe = [&] {
    check(utils::geom::aabb<v2, 2>{ { 5, 5 }, { 15, 15 } });
    check(utils::geom::sphere<v2, 2>{ { 10, 10 }, 6.0f });
    check(utils::geom::ray<v2, 2>{ { 0, 10 }, { 1, 0 }, 50.0f }); // ray осмыслен для тел
    check(utils::geom::cylinder<v2, 2>{ { 0, 0 }, { 0.7071f, 0.7071f }, 50.0f, 2.0f });
    check(utils::geom::obb<v2, 2>{ { 15, 15 }, { 5, 2 }, { v2{ 0.6f, 0.8f }, v2{ -0.8f, 0.6f } } });
  };
  probe();

  SUBCASE("remove + update keep handles valid and results correct") {
    for (int i = 0; i < 60; i += 4) tree.remove(h[i]);
    ref.erase(std::remove_if(ref.begin(), ref.end(), [](auto& e) { return e.first % 4 == 0; }), ref.end());
    REQUIRE(tree.size() == ref.size());
    probe();

    for (int i = 0; i < 60; ++i) { if (i % 4 == 0) continue; const box2 nb = mk(i, 3.0f, -4.0f); tree.update(h[i], nb); for (auto& e : ref) if (e.first == uint32_t(i)) e.second = nb; }
    for (int i = 0; i < 60; ++i) if (i % 4 != 0) REQUIRE(tree.alive(h[i]));
    probe();

    tree.rebuild(); // инвалидирует хендлы, но query остаётся корректным
    probe();
  }

  SUBCASE("empty tree query is a no-op") {
    utils::aabb_tree<pl, v2, 2> empty;
    int n = 0;
    empty.query(utils::geom::sphere<v2, 2>{ { 0, 0 }, 100.0f }, [&](const pl&) { ++n; });
    REQUIRE(n == 0);
  }
}

TEST_CASE("geom::inflate — point store with per-object radius [utils::geom]") {
  using v2 = std::array<float, 2>;
  struct pl { uint32_t id; };
  // объекты как точки, но у каждого «тело» радиуса R; запрос — раздутой формой.
  const float R = 1.0f;
  std::vector<v2> pts = { { 0, 0 }, { 5, 0 }, { 2.5f, 0 }, { -3, 3 }, { 10, 10 } };
  utils::kd_tree<pl, v2, 2> kd;
  for (int i = 0; i < int(pts.size()); ++i) kd.insert(pts[i], pl{ uint32_t(i) });
  kd.build();

  // «Тело радиуса R пересекает сферу s» ⟺ «центр внутри inflate(s, R)».
  const utils::geom::sphere<v2, 2> s{ { 3.5f, 0 }, 1.0f };
  std::vector<uint32_t> got, brute;
  kd.query(utils::geom::inflate(s, R), [&](const pl& p) { got.push_back(p.id); });
  for (int i = 0; i < int(pts.size()); ++i) { // эталон: пересечение диска(pt,R) со сферой s
    const float dx = pts[i][0] - s.center[0], dy = pts[i][1] - s.center[1];
    if (dx * dx + dy * dy <= (s.radius + R) * (s.radius + R)) brute.push_back(uint32_t(i));
  }
  std::sort(got.begin(), got.end()); std::sort(brute.begin(), brute.end());
  REQUIRE(got == brute);

  // inflate(ray) → cylinder радиуса R (толстый луч по точкам).
  const auto thick = utils::geom::inflate(utils::geom::ray<v2, 2>{ { -10, 0 }, { 1, 0 }, 100.0f }, R);
  int n = 0;
  kd.query(thick, [&](const pl&) { ++n; });
  REQUIRE(n == 3); // (0,0),(5,0),(2.5,0) в пределах R от оси y=0; (-3,3),(10,10) — нет
}
