#include <doctest/doctest.h>
#include "devils_engine/utils/stack_allocator.h"

TEST_CASE("fixed pool move transfers allocated and free blocks without rebuilding source") {
  using devils_engine::utils::fixed_pool_mt;
  fixed_pool_mt first(256, 32, 16);
  void* issued = first.allocate();
  REQUIRE(issued != nullptr);
  fixed_pool_mt second(std::move(first));
  CHECK(first.size() == 0);
  CHECK(first.allocate() == nullptr);
  second.free(issued);
  CHECK(second.allocate() == issued);
  second.free(issued);
  fixed_pool_mt third(64, 32, 16);
  third = std::move(second);
  CHECK(second.allocate() == nullptr);
  CHECK(third.allocate() == issued);
  third.free(issued);
  auto* self = &third;
  third = std::move(*self);
  CHECK(third.allocate() == issued);
  third.free(issued);
  first = std::move(third);
  CHECK(first.size() == 256);
  CHECK(first.allocate() == issued);
}
