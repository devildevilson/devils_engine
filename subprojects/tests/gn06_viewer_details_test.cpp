#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../playgrounds/GN06_dungeon_layout/src/viewer_detail_page.h"

namespace {

constexpr size_t line_limit = 14;

std::vector<std::string> detail_lines(const size_t count) {
  std::vector<std::string> lines;
  for (size_t i = 0; i < count; ++i) {
    lines.push_back("line " + std::to_string(i));
  }
  return lines;
}

} // namespace

TEST_CASE("GN06 short property lists fit without a navigation row") {
  for (size_t count = 0; count <= line_limit; ++count) {
    const auto lines = detail_lines(count);
    const auto page = gn06::make_viewer_detail_page(lines, 0, line_limit);
    CHECK(page.lines == lines);
    CHECK(page.index == 0);
    CHECK(page.count == 1);
  }
}

TEST_CASE("GN06 paginates all properties including the reported 18 and 26 line lists") {
  for (const size_t count : {15, 18, 24, 25, 26, 100}) {
    const auto lines = detail_lines(count);
    const auto first = gn06::make_viewer_detail_page(lines, 0, line_limit);
    REQUIRE(first.count > 1);
    std::vector<std::string> recovered{lines[0], lines[1]};
    for (size_t index = 0; index < first.count; ++index) {
      const auto page = gn06::make_viewer_detail_page(lines, index, line_limit);
      REQUIRE(page.lines.size() <= line_limit);
      REQUIRE(page.lines.size() >= 4);
      CHECK(page.index == index);
      CHECK(page.count == first.count);
      CHECK(page.lines[0] == lines[0]);
      CHECK(page.lines[1] == lines[1]);
      CHECK(page.lines.back().find("PgUp/PgDn") != std::string::npos);
      recovered.insert(recovered.end(), page.lines.begin() + 2, page.lines.end() - 1);
    }
    CHECK(recovered == lines);
  }
}

TEST_CASE("GN06 property pages clamp at the last page without wrapping or overflow") {
  const auto lines = detail_lines(26);
  const auto first = gn06::make_viewer_detail_page(lines, 0, line_limit);
  const auto last = gn06::make_viewer_detail_page(lines, first.count - 1, line_limit);
  const auto past_end = gn06::make_viewer_detail_page(lines,
    std::numeric_limits<size_t>::max(), line_limit);
  CHECK(past_end.index == last.index);
  CHECK(past_end.lines == last.lines);
}

TEST_CASE("GN06 replacing a long property list with a short one clears pagination") {
  const auto lines = detail_lines(26);
  const auto last = gn06::make_viewer_detail_page(lines, 2, line_limit);
  REQUIRE(last.index != 0);
  const auto shorter = detail_lines(5);
  const auto page = gn06::make_viewer_detail_page(shorter, last.index, line_limit);
  CHECK(page.index == 0);
  CHECK(page.count == 1);
  CHECK(page.lines == shorter);
}

TEST_CASE("GN06 property pages respect the declared panel capacity") {
  const auto lines = detail_lines(26);
  for (size_t limit = 4; limit <= 30; ++limit) {
    const auto first = gn06::make_viewer_detail_page(lines, 0, limit);
    for (size_t index = 0; index < first.count; ++index) {
      CHECK(gn06::make_viewer_detail_page(lines, index, limit).lines.size() <= limit);
    }
  }
  for (size_t limit = 0; limit < 4; ++limit) {
    CHECK_THROWS_AS(gn06::make_viewer_detail_page(lines, 0, limit), std::invalid_argument);
  }
}
