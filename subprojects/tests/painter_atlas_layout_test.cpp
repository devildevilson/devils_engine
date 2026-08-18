#include <array>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/painter/atlas_layout.h"
#include "devils_engine/painter/common.h"

using namespace devils_engine;

TEST_CASE("atlas layout packs equal regions in request order [painter]") {
  const std::array<uint32_t, 4> sizes{1024, 1024, 1024, 1024};
  std::array<painter::atlas_region, 4> regions{};
  REQUIRE(painter::allocate_atlas_regions(2048, 2048, sizes, regions));

  // Прежняя жёсткая раскладка 2x2 должна воспроизводиться ровно: контракт не меняет картинку там,
  // где раньше всё было захардкожено.
  CHECK(regions[0].x == 0);
  CHECK(regions[0].y == 0);
  CHECK(regions[1].x == 1024);
  CHECK(regions[1].y == 0);
  CHECK(regions[2].x == 0);
  CHECK(regions[2].y == 1024);
  CHECK(regions[3].x == 1024);
  CHECK(regions[3].y == 1024);
  CHECK(painter::atlas_occupancy(regions, 2048, 2048) == doctest::Approx(1.0f));
}

TEST_CASE("atlas layout spends budget unequally without overlap [painter]") {
  // Ближнему каскаду нужен крупный регион, хвосту — мелкие: это и есть смысл контракта.
  const std::array<uint32_t, 6> sizes{2048, 1024, 1024, 512, 512, 512};
  std::array<painter::atlas_region, 6> regions{};
  REQUIRE(painter::allocate_atlas_regions(4096, 2048, sizes, regions));

  for (size_t i = 0; i < regions.size(); ++i) {
    CHECK(regions[i].size == sizes[i]);
    CHECK(regions[i].x + regions[i].size <= 4096);
    CHECK(regions[i].y + regions[i].size <= 2048);
    for (size_t j = i + 1; j < regions.size(); ++j) {
      const bool disjoint_x = regions[i].x + regions[i].size <= regions[j].x ||
                              regions[j].x + regions[j].size <= regions[i].x;
      const bool disjoint_y = regions[i].y + regions[i].size <= regions[j].y ||
                              regions[j].y + regions[j].size <= regions[i].y;
      CHECK((disjoint_x || disjoint_y));
    }
  }
}

TEST_CASE("atlas layout reports failure instead of overlapping [painter]") {
  const std::array<uint32_t, 5> sizes{1024, 1024, 1024, 1024, 1024};
  std::array<painter::atlas_region, 5> regions{};
  CHECK_FALSE(painter::allocate_atlas_regions(2048, 2048, sizes, regions));

  const std::array<uint32_t, 1> too_big{4096};
  std::array<painter::atlas_region, 1> single{};
  CHECK_FALSE(painter::allocate_atlas_regions(2048, 2048, too_big, single));

  const std::array<uint32_t, 1> zero{0};
  CHECK_FALSE(painter::allocate_atlas_regions(2048, 2048, zero, single));

  std::array<painter::atlas_region, 1> short_output{};
  CHECK_FALSE(painter::allocate_atlas_regions(2048, 2048, sizes, short_output));
}

TEST_CASE("atlas layout is deterministic and yields matching uv transforms [painter]") {
  const std::array<uint32_t, 5> sizes{512, 2048, 512, 1024, 512};
  std::array<painter::atlas_region, 5> first{};
  std::array<painter::atlas_region, 5> second{};
  REQUIRE(painter::allocate_atlas_regions(4096, 4096, sizes, first));
  REQUIRE(painter::allocate_atlas_regions(4096, 4096, sizes, second));
  for (size_t i = 0; i < first.size(); ++i) {
    CHECK(first[i].x == second[i].x);
    CHECK(first[i].y == second[i].y);
    CHECK(first[i].size == second[i].size);
  }

  const auto uv = painter::atlas_region_uv(first[3], 4096, 4096);
  CHECK(uv.scale_x == doctest::Approx(float(first[3].size) / 4096.0f));
  CHECK(uv.offset_x == doctest::Approx(float(first[3].x) / 4096.0f));
  CHECK(uv.offset_y == doctest::Approx(float(first[3].y) / 4096.0f));

  // Центр региона обязан лежать внутри своего окна в atlas-uv.
  const float center_u = 0.5f * uv.scale_x + uv.offset_x;
  CHECK(center_u > uv.offset_x);
  CHECK(center_u < uv.offset_x + uv.scale_x);
}

TEST_CASE("history copy index keeps shader-side indices constant [painter]") {
  // Контракт: [0] всегда текущая копия, [1] — предыдущий кадр, [2] — позапрошлый, при ЛЮБОМ значении
  // счётчика кадров. Именно это позволяет шейдеру брать историю по константному индексу.
  for (uint32_t clock = 0; clock < 12; ++clock) {
    CHECK(painter::history_copy_index(clock, 0, 3) == clock % 3);
    CHECK(painter::history_copy_index(clock, 1, 3) == (clock + 2) % 3);
    CHECK(painter::history_copy_index(clock, 2, 3) == (clock + 1) % 3);

    // Разные копии для разной давности, и полный оборот возвращает текущую.
    CHECK(painter::history_copy_index(clock, 1, 3) != painter::history_copy_index(clock, 0, 3));
    CHECK(painter::history_copy_index(clock, 2, 3) != painter::history_copy_index(clock, 1, 3));
    CHECK(painter::history_copy_index(clock, 3, 3) == painter::history_copy_index(clock, 0, 3));
  }

  // Двойная буферизация: одна история, чередование без вырождения.
  CHECK(painter::history_copy_index(7, 0, 2) == 1);
  CHECK(painter::history_copy_index(7, 1, 2) == 0);
  // Одна копия: истории нет, всё указывает на неё же — это не ошибка, а отсутствие истории.
  CHECK(painter::history_copy_index(5, 1, 1) == 0);
  CHECK(painter::history_copy_index(5, 1, 0) == 0);
}
