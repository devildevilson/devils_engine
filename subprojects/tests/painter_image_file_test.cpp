#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/painter/image_file.h"

namespace painter = devils_engine::painter;

namespace {

// 2x1: красный и синий пиксели, альфа служебная (не должна попасть в файл).
constexpr std::array<uint8_t, 8> rgba_pixels{255, 0, 0, 7, 0, 0, 255, 9};
constexpr std::array<uint8_t, 8> bgra_pixels{0, 0, 255, 7, 255, 0, 0, 9};

painter::image_view view_of(const std::array<uint8_t, 8>& pixels, const bool bgra) {
  return painter::image_view{2, 1, bgra, std::span<const uint8_t>(pixels.data(), pixels.size())};
}

} // namespace

TEST_CASE("image file format follows the extension [painter::image_file]") {
  CHECK(painter::image_file_format_from_path("a/b.PNG") == painter::image_file_format::png);
  CHECK(painter::image_file_format_from_path("x.avif") == painter::image_file_format::avif);
  CHECK(painter::image_file_format_from_path("x.ppm") == painter::image_file_format::ppm);
  CHECK_FALSE(painter::image_file_format_from_path("x.jpg").has_value());
  CHECK_FALSE(painter::image_file_format_from_path("png").has_value());
  CHECK(painter::image_file_format_available(painter::image_file_format::png));
  CHECK(painter::image_file_format_available(painter::image_file_format::ppm));
}

TEST_CASE("ppm is RGB without alpha in both channel orders [painter::image_file]") {
  const std::string header = "P6\n2 1\n255\n";
  std::vector<uint8_t> expected(header.begin(), header.end());
  for (const uint8_t b : {255, 0, 0, 0, 0, 255}) expected.push_back(b);

  for (const bool bgra : {false, true}) {
    const auto bytes = painter::encode_image(painter::image_file_format::ppm, view_of(bgra ? bgra_pixels : rgba_pixels, bgra));
    REQUIRE(bytes.has_value());
    CHECK(*bytes == expected);
  }
}

TEST_CASE("png and avif produce their containers [painter::image_file]") {
  const auto png = painter::encode_image(painter::image_file_format::png, view_of(rgba_pixels, false));
  REQUIRE(png.has_value());
  REQUIRE(png->size() > 8);
  CHECK(std::memcmp(png->data(), "\x89PNG\r\n\x1a\n", 8) == 0);

  const auto avif = painter::encode_image(painter::image_file_format::avif, view_of(rgba_pixels, false));
  if (painter::image_file_format_available(painter::image_file_format::avif)) {
    REQUIRE(avif.has_value());
    REQUIRE(avif->size() > 12);
    CHECK(std::memcmp(avif->data() + 4, "ftyp", 4) == 0);
  } else {
    CHECK_FALSE(avif.has_value()); // отказ-статус, не падение
  }
}

TEST_CASE("bad input and unknown extensions are refused with a reason [painter::image_file]") {
  const std::array<uint8_t, 4> short_pixels{};
  const painter::image_view short_view{2, 1, false, std::span<const uint8_t>(short_pixels.data(), short_pixels.size())};
  CHECK_FALSE(painter::encode_image(painter::image_file_format::png, short_view).has_value());
  CHECK_FALSE(painter::encode_image(painter::image_file_format::ppm, painter::image_view{}).has_value());
  const auto written = painter::write_image_file("frame.jpg", view_of(rgba_pixels, false));
  REQUIRE_FALSE(written.has_value());
  CHECK(written.error().find("unknown image extension") != std::string::npos);
}
