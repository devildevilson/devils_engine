#include <catch2/catch_test_macros.hpp>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/list.h"
#include "devils_engine/utils/block_allocator.h"
#include "devils_engine/utils/string-utils.hpp"
#include "devils_engine/mood/system.h"

using namespace devils_engine;

enum list_values {
  list_type_1,
  list_type_2,
  list_type_3,
};

struct list_test_1_t : public utils::ring::list<list_test_1_t, list_type_1> {};
struct list_test_2_t : public utils::ring::list<list_test_2_t, list_type_2>, public utils::ring::list<list_test_2_t, list_type_3> {};

struct list_test4_t : public utils::forw::list<list_test4_t, list_type_1> {};

TEST_CASE("Single and double linked lists", "[list]") {
  SECTION("ring list usage") {
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

  SECTION("forw list usage") {
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

TEST_CASE("Block allocator tests", "[block_allocator]") {
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

TEST_CASE("Mood system tests", "[mood::system]") {
  const std::initializer_list<std::string_view> a_strs = { "action1", "action2", "action3", "action4", "action5" };
  const std::initializer_list<std::string_view> g_strs = { "guard1", "guard2", "guard3", "guard4", "guard5", "guard6", };

  mood::table t;
  for (const auto& name : a_strs) {
    t.actions[name] = [] (void*) { return 0; };
  }

  for (const auto& name : g_strs) {
    t.guards[name] = [] (void*) { return 0; };
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
  REQUIRE(trans5[0].current_state_on_exit.size() == 1); // convenient way to find melee_attack + on_exit
  REQUIRE(trans5[0].next_state_on_entry.size() == 0);
  const auto trans6 = s.find_transitions("melee_ready", "attack");
  REQUIRE(trans6.size() == 1);
  REQUIRE(trans6[0].next_state == "melee_attack");
  REQUIRE(trans6[0].current_state_on_exit.size() == 0);
  REQUIRE(trans6[0].next_state_on_entry.size() == 1); // convenient way to find melee_attack + on_entry
}

TEST_CASE("String utility tests", "[utils::string]") {
  SECTION("split test normal usage 'abc.ab.rtetr.bac.wert'") {
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
  
  SECTION("split test abnormal usage 'qwertyuiop', 'asd.....asd.....asd', ''") {
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

  SECTION("split test different token usage") {
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

  SECTION("inside2 test") {
    const std::string_view test1 = "{ \"abbbaab\": \"abc\", \"babav\": [ \"ababab\", 123, { \"bfsdb\": [ \"abc\" ] } ] }";
    const auto ret = utils::string::trim(utils::string::inside2(test1, "[", "]"));
    REQUIRE(ret == "\"ababab\", 123, { \"bfsdb\": [ \"abc\" ] }");
  }

  SECTION("trim test") {
    REQUIRE(utils::string::trim("   a    ") == "a");
    REQUIRE(utils::string::trim("   a") == "a");
    REQUIRE(utils::string::trim("a    ") == "a");
    REQUIRE(utils::string::trim("a") == "a");
    REQUIRE(utils::string::trim("\n\t\v\ra    ") == "a");
  }

  SECTION("stoi test") {
    REQUIRE(utils::string::stoi("10") == 10);
    REQUIRE(utils::string::stoi("34634634") == 34634634);
    REQUIRE(utils::string::stoi("1") == 1);
    REQUIRE(utils::string::stoi("") == 0);
    REQUIRE(utils::string::stoi("a1") == 0);
    REQUIRE(utils::string::stoi("-1") == 0);
  }

  SECTION("slice test") {
    REQUIRE(utils::string::slice("qwertyuiop") == "qwertyuiop");
    REQUIRE(utils::string::slice("qwertyuiop", 1) == "wertyuiop");
    REQUIRE(utils::string::slice("qwertyuiop", 1, 1) == "");
    REQUIRE(utils::string::slice("qwertyuiop", 1, 2) == "w");
    REQUIRE(utils::string::slice("qwertyuiop", -5, -2) == "yui");
    REQUIRE(utils::string::slice("qwertyuiop", 3, -3) == "rtyu");
  }

  SECTION("find_ci test") {
    REQUIRE(utils::string::find_ci("QWERTYUIOP", "qwe") == 0);
    REQUIRE(utils::string::find_ci("QwErTyUiOp", "qWe") == 0);
    REQUIRE(utils::string::find_ci("QwErTyUiOp", "ert") == 2);
  }
}
