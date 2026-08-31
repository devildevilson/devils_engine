#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/buffer.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

originator::buffer make_tiles(const originator::storage_kind::values storage, const size_t count) {
  const std::vector<field_pair> fields = {
    {"height", "v1"},
    {"temperature", "c1"},
    {"owner", "us1"},
    {"direction", "v3"},
  };
  auto layout = originator::make_buffer_layout(storage, fields, "tiles");
  return originator::buffer("tiles", std::move(layout), count);
}
} // namespace

TEST_CASE("originator field type spelling follows painter") {
  CHECK(originator::parse_field_type("v3").components == 3);
  CHECK(originator::parse_field_type("v3").byte_size() == 12);
  CHECK(originator::parse_field_type("ui1").base == originator::field_base::ui);
  CHECK(originator::parse_field_type("us1").byte_size() == 2);
  CHECK(originator::parse_field_type("c4").byte_size() == 4);
  CHECK(originator::parse_field_type("sf1").byte_size() == 2);
  CHECK(originator::parse_field_type("is2").kind() == originator::field_kind::signed_integer);

  // Мусор не превращается в валидный тип молча.
  CHECK_FALSE(originator::parse_field_type("v").valid());
  CHECK_FALSE(originator::parse_field_type("3").valid());
  CHECK_FALSE(originator::parse_field_type("zz2").valid());
  CHECK_FALSE(originator::parse_field_type("v9").valid());
  CHECK_FALSE(originator::parse_field_type("").valid());
}

TEST_CASE("originator half storage round-trips through the canonical double") {
  const std::vector<field_pair> fields = {{"h", "sf1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "half");
  originator::buffer b("half", std::move(layout), 8);
  auto h = b.field(0);

  h.set(0, 1.0);
  h.set(1, -2.5);
  h.set(2, 0.0);
  h.set(3, 65504.0); // максимум f16

  CHECK(h.get(0) == doctest::Approx(1.0));
  CHECK(h.get(1) == doctest::Approx(-2.5));
  CHECK(h.get(2) == doctest::Approx(0.0));
  CHECK(h.get(3) == doctest::Approx(65504.0));

  // Малые значения теряют точность, но не превращаются в мусор.
  h.set(4, 0.333);
  CHECK(h.get(4) == doctest::Approx(0.333).epsilon(0.001));
}

TEST_CASE("originator aos and soa are indistinguishable through named access") {
  constexpr size_t count = 64;

  for (const auto storage : {originator::storage_kind::aos, originator::storage_kind::soa}) {
    auto b = make_tiles(storage, count);

    const size_t height = b.find_field("height");
    const size_t owner = b.find_field("owner");
    const size_t direction = b.find_field("direction");
    REQUIRE(height != originator::buffer_layout::npos);
    REQUIRE(owner != originator::buffer_layout::npos);
    REQUIRE(b.find_field("missing") == originator::buffer_layout::npos);

    auto h = b.field(height);
    auto o = b.field(owner);
    auto d = b.field(direction);

    for (size_t i = 0; i < count; ++i) {
      h.set(i, double(i) * 0.5);
      o.set(i, double(i % 7));
      d.set(i, double(i), 0);
      d.set(i, double(i) + 1.0, 1);
      d.set(i, double(i) + 2.0, 2);
    }

    for (size_t i = 0; i < count; ++i) {
      CHECK(h.get(i) == doctest::Approx(double(i) * 0.5));
      CHECK(o.get(i) == doctest::Approx(double(i % 7)));
      CHECK(d.get(i, 0) == doctest::Approx(double(i)));
      CHECK(d.get(i, 2) == doctest::Approx(double(i) + 2.0));
    }

    // Выход за границы не пишет чужую память и не читает мусор.
    h.set(count, 1234.0);
    CHECK(h.get(count) == doctest::Approx(0.0));
    CHECK(h.get(0, 3) == doctest::Approx(0.0));
  }
}

TEST_CASE("originator soa gives kernels a contiguous span, aos does not") {
  constexpr size_t count = 32;

  auto soa = make_tiles(originator::storage_kind::soa, count);
  auto h_soa = soa.field(soa.find_field("height"));
  CHECK(h_soa.contiguous());
  const auto span = h_soa.as_span<float>();
  REQUIRE(span.size() == count);

  for (size_t i = 0; i < count; ++i) {
    span[i] = float(i) * 2.0f;
  }
  CHECK(h_soa.get(5) == doctest::Approx(10.0));

  auto aos = make_tiles(originator::storage_kind::aos, count);
  auto h_aos = aos.field(aos.find_field("height"));
  CHECK_FALSE(h_aos.contiguous());
  CHECK(h_aos.as_span<float>().empty());
  CHECK(h_aos.stride() == aos.layout().element_byte_size());
}

TEST_CASE("originator layout reports honest memory cost") {
  constexpr size_t count = 1000;

  auto aos = make_tiles(originator::storage_kind::aos, count);
  // v1 + c1 + us1 + v3 = 4 + 1 + (пад 1) + 2 + 12 = 20, выравнивание элемента 4.
  CHECK(aos.layout().element_byte_size() == 20);
  CHECK(aos.byte_size() == 20 * count);

  auto soa = make_tiles(originator::storage_kind::soa, count);
  // Планы выровнены на строку кэша, поэтому soa чуть больше суммы полей, но не в разы.
  CHECK(soa.byte_size() >= 19 * count);
  CHECK(soa.byte_size() < 20 * count + 4 * 64);
}

TEST_CASE("originator read binding cannot write") {
  auto b = make_tiles(originator::storage_kind::soa, 4);
  const auto& const_ref = b;

  originator::const_field_accessor read = const_ref.field(0);
  CHECK(read.valid());

  // Изменяемый вид сужается до неизменяемого, обратное преобразование не существует.
  originator::field_accessor write = b.field(0);
  originator::const_field_accessor narrowed = write;
  write.set(1, 3.0);
  CHECK(narrowed.get(1) == doctest::Approx(3.0));

  static_assert(!std::is_constructible_v<originator::field_accessor, originator::const_field_accessor>,
                "read binding must not be widenable to a write binding");
}
