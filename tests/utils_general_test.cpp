#include <catch2/catch_test_macros.hpp>

#include "utils/core.h"
#include "utils/list.h"
#include "utils/block_allocator.h"

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
  REQUIRE(al.allocation_size() == block_size);
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

