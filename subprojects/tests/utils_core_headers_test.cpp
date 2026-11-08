#include <doctest/doctest.h>
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/crc32c.h"
#include "devils_engine/utils/log.h"
#include "devils_engine/utils/numeric.h"
#include "devils_engine/utils/platform.h"
#include "devils_engine/utils/print.h"
#include "devils_engine/utils/trace.h"
#include "devils_engine/utils/unicode.h"

namespace {
constexpr int checked_constant(int value) {
  if (value < 0) devils_engine::utils::error{}("negative value {}", value);
  return value;
}
static_assert(checked_constant(3) == 3);
static_assert(std::is_same_v<decltype(devils_engine::utils::min(1, 2)), int>);
} // namespace
TEST_CASE("narrow utils headers preserve diagnostic numeric unicode and CRC contracts") {
  using namespace devils_engine;
  CHECK(utils::make_sane_file_name("/root/area/file.cpp") == "area/file.cpp");
  CHECK_THROWS_AS(checked_constant(-1), utils::critical_error);
  CHECK(utils::min(4, 1, 3) == 1);
  CHECK(utils::align_to(17, 16) == 32);
  CHECK(utils::cast16("abc") == u"abc");
  CHECK(utils::crc32c(std::string_view("123456789")) == 0xe3069283u);
}
