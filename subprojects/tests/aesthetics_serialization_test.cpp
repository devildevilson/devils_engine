#include <doctest/doctest.h>

#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <string>

#include <gtl/phmap.hpp>

#include "devils_engine/aesthetics/world.h"
#include "devils_engine/aesthetics/serialization.h"
#include "devils_engine/aesthetics/sink.h"

using namespace devils_engine;

namespace {

struct pos { int32_t x = 0; int32_t y = 0; };
struct vel { float dx = 0.0f; float dy = 0.0f; };
struct link { aesthetics::entityid_t target = aesthetics::invalid_entityid; }; // ссылка на другую энтити

// сложный агрегат: контейнеры/строка/фикс-массив/владеющий указатель/хеш-мапа
struct rich {
  std::vector<int32_t> ints;
  std::string name;
  std::array<float, 3> vec;
  std::unique_ptr<int32_t> owned;
  gtl::flat_hash_map<int32_t, int32_t> table;
};

SERIALIZABLE_COMPONENT(pos)
SERIALIZABLE_COMPONENT(vel)
SERIALIZABLE_COMPONENT(link)
SERIALIZABLE_COMPONENT(rich)

// внешний НЕ-агрегат (имитация glm::vec: есть пользовательские ctor'ы) — reflect его не осилит,
// сериализуется через specialization adapter'а (без зависимости aesthetics на glm).
struct vec3f {
  float x, y, z;
  vec3f() : x(0), y(0), z(0) {}
  vec3f(float a, float b, float c) : x(a), y(b), z(c) {}
};

} // namespace

namespace devils_engine::aesthetics::serial {
template <> struct adapter<vec3f> {
  static constexpr std::string_view name = "test.vec3f"; // кросс-компиляторный тег
  static void write(writer& w, const vec3f& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
  static void read(reader& r, vec3f& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }
};
} // namespace devils_engine::aesthetics::serial

namespace {

struct transform { vec3f pos; float scale = 1.0f; }; // агрегат, содержащий адаптируемый тип

struct load_watcher : aesthetics::basic_reciever<aesthetics::serial::snapshot_loaded_event> {
  int fired = 0;
  void receive(const aesthetics::serial::snapshot_loaded_event&) override { ++fired; }
};

} // namespace

SERIALIZABLE_COMPONENT(transform)

TEST_CASE("snapshot round-trips components, entity refs and generator state [aesthetics::serial]") {
  aesthetics::world w;

  const auto e0 = w.gen_entityid();
  const auto e1 = w.gen_entityid();
  const auto e2 = w.gen_entityid();

  w.create<pos>(e0, pos{1, 2});
  w.create<vel>(e0, vel{0.5f, -0.5f});
  w.create<pos>(e1, pos{3, 4});
  w.create<link>(e2, link{e0}); // держит полный entity_id (с версией) на e0

  w.remove_entity(e1); // e1 уходит во free-list -> проверяем и состояние генератора, и что удалённое не сериализуется

  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  aesthetics::world w2;
  aesthetics::serial::in_t in{buf};
  REQUIRE(aesthetics::serial::load_world(&w2, in));

  SUBCASE("components restored") {
    const auto* p0 = w2.get<pos>(e0);
    REQUIRE(p0 != nullptr);
    CHECK(p0->x == 1);
    CHECK(p0->y == 2);

    const auto* v0 = w2.get<vel>(e0);
    REQUIRE(v0 != nullptr);
    CHECK(v0->dx == doctest::Approx(0.5f));
    CHECK(v0->dy == doctest::Approx(-0.5f));
  }

  SUBCASE("entity reference stays valid (full id incl. version restored)") {
    const auto* l = w2.get<link>(e2);
    REQUIRE(l != nullptr);
    CHECK(l->target == e0);
    CHECK(w2.get<pos>(l->target) != nullptr); // ссылка реально разрешается в живую энтити
  }

  SUBCASE("removed entity is not present in the snapshot") {
    CHECK(w2.get<pos>(e1) == nullptr);
  }

  SUBCASE("generator continues exactly from restored state") {
    // free-list содержал e1 -> следующий id переиспользует его индекс с версией+1,
    // одинаково в оригинале и в загруженном мире.
    CHECK(w2.gen_entityid() == w.gen_entityid());
  }
}

TEST_CASE("schema/magic guard rejects incompatible snapshot without throwing [aesthetics::serial]") {
  aesthetics::world w;
  w.create<pos>(w.gen_entityid(), pos{7, 8});

  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  SUBCASE("corrupted fingerprint -> false") {
    auto bad = buf;
    bad[4] = std::byte(std::to_integer<uint8_t>(bad[4]) ^ 0xFF); // fingerprint = байты [4..8)
    aesthetics::world w2;
    aesthetics::serial::in_t in{bad};
    CHECK_FALSE(aesthetics::serial::load_world(&w2, in));
  }

  SUBCASE("corrupted magic -> false") {
    auto bad = buf;
    bad[0] = std::byte(std::to_integer<uint8_t>(bad[0]) ^ 0xFF); // magic = байты [0..4)
    aesthetics::world w2;
    aesthetics::serial::in_t in{bad};
    CHECK_FALSE(aesthetics::serial::load_world(&w2, in));
  }
}

TEST_CASE("complex aggregate (vector/string/array/unique_ptr/flat_hash_map) round-trips [aesthetics::serial]") {
  aesthetics::world w;
  const auto e = w.gen_entityid();
  auto* r = w.create<rich>(e);
  REQUIRE(r != nullptr);
  r->ints = {1, 2, 3, 4};
  r->name = "hello world";
  r->vec = {1.5f, 2.5f, 3.5f};
  r->owned = std::make_unique<int32_t>(42);
  r->table[7] = 70;
  r->table[9] = 90;

  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  aesthetics::world w2;
  aesthetics::serial::in_t in{buf};
  REQUIRE(aesthetics::serial::load_world(&w2, in));

  const auto* r2 = w2.get<rich>(e);
  REQUIRE(r2 != nullptr);
  CHECK(r2->ints == std::vector<int32_t>{1, 2, 3, 4});
  CHECK(r2->name == "hello world");
  CHECK(r2->vec[0] == doctest::Approx(1.5f));
  CHECK(r2->vec[2] == doctest::Approx(3.5f));
  REQUIRE(r2->owned != nullptr);
  CHECK(*r2->owned == 42);
  CHECK(r2->table.size() == 2);
  CHECK(r2->table.at(7) == 70);
  CHECK(r2->table.at(9) == 90);
}

TEST_CASE("adapter serializes external non-aggregate type (glm-like) [aesthetics::serial]") {
  aesthetics::world w;
  const auto e = w.gen_entityid();
  auto* t = w.create<transform>(e);
  t->pos = vec3f{1.5f, 2.5f, 3.5f};
  t->scale = 4.0f;

  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  aesthetics::world w2;
  aesthetics::serial::in_t in{buf};
  REQUIRE(aesthetics::serial::load_world(&w2, in));

  const auto* t2 = w2.get<transform>(e);
  REQUIRE(t2 != nullptr);
  CHECK(t2->pos.x == doctest::Approx(1.5f));
  CHECK(t2->pos.y == doctest::Approx(2.5f));
  CHECK(t2->pos.z == doctest::Approx(3.5f));
  CHECK(t2->scale == doctest::Approx(4.0f));
}

TEST_CASE("load_world emits snapshot_loaded_event (signal to rebuild queries) [aesthetics::serial]") {
  aesthetics::world w;
  w.create<pos>(w.gen_entityid(), pos{1, 2});
  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  aesthetics::world w2;
  load_watcher watcher;
  w2.subscribe<aesthetics::serial::snapshot_loaded_event>(&watcher);
  aesthetics::serial::in_t in{buf};
  REQUIRE(aesthetics::serial::load_world(&w2, in));
  CHECK(watcher.fired == 1);
  w2.unsubscribe<aesthetics::serial::snapshot_loaded_event>(&watcher);
}

TEST_CASE("empty world round-trips [aesthetics::serial]") {
  aesthetics::world w;
  std::vector<std::byte> buf;
  aesthetics::serial::out_t out{buf};
  aesthetics::serial::dump_world(&w, out);

  aesthetics::world w2;
  aesthetics::serial::in_t in{buf};
  CHECK(aesthetics::serial::load_world(&w2, in));
}

TEST_CASE("hash-map serializes deterministically regardless of capacity/order [aesthetics::serial]") {
  // одинаковое содержимое, разный capacity -> разный порядок итерации flat_hash_map;
  // сортировка по ключу в serialize_det должна дать ИДЕНТИЧНЫЕ байты.
  gtl::flat_hash_map<int32_t, int32_t> a;
  gtl::flat_hash_map<int32_t, int32_t> b;
  b.reserve(1024);
  for (int32_t i = 0; i < 100; ++i) { a[i] = i * 7; b[i] = i * 7; }

  std::vector<std::byte> ba, bb;
  aesthetics::serial::writer oa{ba};
  aesthetics::serial::writer ob{bb};
  aesthetics::serial::serialize(oa, a);
  aesthetics::serial::serialize(ob, b);
  CHECK(ba == bb);
}

TEST_CASE("sink pack/unpack round-trips with compression + checksum [aesthetics::serial]") {
  aesthetics::world w;
  const auto e = w.gen_entityid();
  w.create<pos>(e, pos{11, 22});
  auto* r = w.create<rich>(e);
  r->name = "sink test";
  r->ints = {5, 6, 7};
  r->table[1] = 10;

  SUBCASE("network policy (light compression, no screenshot)") {
    const auto bytes = aesthetics::serial::pack(&w, aesthetics::serial::network_policy);
    aesthetics::world w2;
    REQUIRE(aesthetics::serial::unpack(bytes, &w2));
    CHECK(w2.get<pos>(e)->x == 11);
    CHECK(w2.get<rich>(e)->name == "sink test");
    CHECK(w2.get<rich>(e)->table.at(1) == 10);
  }

  SUBCASE("disk policy with embedded screenshot") {
    const std::vector<uint8_t> shot = {0x89, 'P', 'N', 'G', 1, 2, 3, 4};
    const auto bytes = aesthetics::serial::pack(&w, aesthetics::serial::disk_policy, shot);
    aesthetics::world w2;
    std::vector<uint8_t> shot_out;
    REQUIRE(aesthetics::serial::unpack(bytes, &w2, &shot_out));
    CHECK(shot_out == shot);
    CHECK(w2.get<pos>(e)->x == 11);
  }

  SUBCASE("corrupted payload fails checksum") {
    auto bytes = aesthetics::serial::pack(&w, aesthetics::serial::network_policy);
    bytes.back() ^= 0xFFu; // портим последний байт payload
    aesthetics::world w2;
    CHECK_FALSE(aesthetics::serial::unpack(bytes, &w2));
  }
}
