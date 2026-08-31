#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/originator/tools.h"
#include "devils_engine/thread/atomic_pool.h"

using namespace devils_engine;

namespace {
using field_pair = std::pair<std::string_view, std::string_view>;

// Больше одного фиксированного чанка (65536), иначе двухфазная схема не проверяется.
constexpr size_t count = 200000;
constexpr size_t bucket_count = 97;

originator::buffer make_elements() {
  const std::vector<field_pair> fields = {{"key", "us1"}, {"value", "v1"}};
  auto layout = originator::make_buffer_layout(originator::storage_kind::soa, fields, "elements");
  originator::buffer elements("elements", std::move(layout), count);

  auto key = elements.field(0);
  auto value = elements.field(1);
  for (size_t i = 0; i < count; ++i) {
    key.set(i, double((i * 31u + i / 977u) % bucket_count));
    value.set(i, double(i % 13) * 0.25);
  }
  return elements;
}

originator::buffer make_buffer(const std::string& name, const std::string_view& type, const size_t size) {
  const std::vector<field_pair> fields = {{"v", type}};
  return originator::buffer(name, originator::make_buffer_layout(originator::storage_kind::soa, fields, name), size);
}

originator::field_ref writable(originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, &b, b.find_field(name)};
}

originator::field_ref readable(const originator::buffer& b, const std::string_view& name) {
  return originator::field_ref{&b, nullptr, b.find_field(name)};
}

originator::tool_registry make_registry() {
  originator::tool_registry registry;
  registry.add_standard_tools();
  return registry;
}
} // namespace

TEST_CASE("originator group_by builds a correct CSR and keeps element order") {
  const auto registry = make_registry();
  const auto* group_by = registry.find("group_by");
  REQUIRE(group_by != nullptr);
  CHECK(group_by->shape == originator::aperture::scatter);

  auto elements = make_elements();
  auto offsets = make_buffer("offsets", "ui1", bucket_count + 1);
  auto indices = make_buffer("indices", "ui1", count);

  const originator::parameters params;
  const std::vector<originator::field_ref> inputs{readable(elements, "key")};
  const std::vector<originator::field_ref> outputs{writable(offsets, "v"), writable(indices, "v")};
  originator::dispatch(*group_by, inputs, outputs, params, 1, 0, count, "grouping", nullptr);

  const auto key = elements.field(0);
  const auto start = offsets.field(0);
  const auto index = indices.field(0);

  CHECK(start.get(0) == 0.0);
  CHECK(start.get(bucket_count) == double(count));

  size_t seen = 0;
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const auto first = size_t(start.get(bucket));
    const auto last = size_t(start.get(bucket + 1));
    REQUIRE(last >= first);
    seen += last - first;

    // Каждый элемент группы действительно имеет её ключ, и порядок внутри группы возрастающий:
    // порядок задан номером чанка и индексом элемента, а не тем, кто когда успел.
    for (size_t k = first; k < last; ++k) {
      const auto element = size_t(index.get(k));
      CHECK(key.get(element) == double(bucket));
      if (k > first) {
        CHECK(index.get(k - 1) < index.get(k));
      }
    }
  }
  CHECK(seen == count);
}

TEST_CASE("originator group_by does not depend on the number of threads") {
  const auto registry = make_registry();
  const auto* group_by = registry.find("group_by");

  auto elements = make_elements();
  const originator::parameters params;
  const std::vector<originator::field_ref> inputs{readable(elements, "key")};

  auto reference_offsets = make_buffer("offsets", "ui1", bucket_count + 1);
  auto reference_indices = make_buffer("indices", "ui1", count);
  const std::vector<originator::field_ref> reference_out{writable(reference_offsets, "v"), writable(reference_indices, "v")};
  originator::dispatch(*group_by, inputs, reference_out, params, 1, 0, count, "grouping", nullptr);

  for (const size_t threads : {size_t(1), size_t(4), size_t(9)}) {
    thread::atomic_pool pool(threads);
    auto offsets = make_buffer("offsets", "ui1", bucket_count + 1);
    auto indices = make_buffer("indices", "ui1", count);
    const std::vector<originator::field_ref> out{writable(offsets, "v"), writable(indices, "v")};
    originator::dispatch(*group_by, inputs, out, params, 1, 0, count, "grouping", &pool);

    bool identical = true;
    for (size_t i = 0; i <= bucket_count; ++i) {
      identical = identical && offsets.field(0).get(i) == reference_offsets.field(0).get(i);
    }
    for (size_t i = 0; i < count; ++i) {
      identical = identical && indices.field(0).get(i) == reference_indices.field(0).get(i);
    }
    CHECK(identical);
  }
}

TEST_CASE("originator accumulate sums per bucket, bit-identically at any thread count") {
  const auto registry = make_registry();
  const auto* accumulate = registry.find("accumulate");
  REQUIRE(accumulate != nullptr);
  CHECK(accumulate->shape == originator::aperture::scatter);

  auto elements = make_elements();
  const originator::parameters params;
  const std::vector<originator::field_ref> inputs{readable(elements, "key"), readable(elements, "value")};

  auto reference = make_buffer("sums", "v1", bucket_count);
  const std::vector<originator::field_ref> reference_out{writable(reference, "v")};
  originator::dispatch(*accumulate, inputs, reference_out, params, 1, 0, count, "totals", nullptr);

  // Суммы совпадают с прямым подсчётом по группам.
  const auto key = elements.field(0);
  const auto value = elements.field(1);
  std::vector<double> expected(bucket_count, 0.0);
  for (size_t i = 0; i < count; ++i) {
    expected[size_t(key.get(i))] += value.get(i);
  }
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    CHECK(reference.field(0).get(bucket) == doctest::Approx(expected[bucket]));
  }

  // Сравнение ТОЧНОЕ: фиксированные чанки и слияние по их порядку дают тот же результат при любом
  // числе потоков, хотя порядок сложения плавающих чисел в принципе значим.
  for (const size_t threads : {size_t(1), size_t(4), size_t(9)}) {
    thread::atomic_pool pool(threads);
    auto sums = make_buffer("sums", "v1", bucket_count);
    const std::vector<originator::field_ref> out{writable(sums, "v")};
    originator::dispatch(*accumulate, inputs, out, params, 1, 0, count, "totals", &pool);

    bool identical = true;
    for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
      identical = identical && sums.field(0).get(bucket) == reference.field(0).get(bucket);
    }
    CHECK(identical);
  }
}

TEST_CASE("originator accumulate can add into existing sums") {
  const auto registry = make_registry();
  const auto* accumulate = registry.find("accumulate");

  auto elements = make_elements();
  const std::vector<originator::field_ref> inputs{readable(elements, "key"), readable(elements, "value")};

  auto sums = make_buffer("sums", "v1", bucket_count);
  const std::vector<originator::field_ref> out{writable(sums, "v")};

  originator::parameters params;
  originator::dispatch(*accumulate, inputs, out, params, 1, 0, count, "totals", nullptr);
  const double first_pass = sums.field(0).get(3);

  params.set_number("reset", 0);
  originator::dispatch(*accumulate, inputs, out, params, 1, 0, count, "totals", nullptr);
  CHECK(sums.field(0).get(3) == doctest::Approx(first_pass * 2.0));
}

TEST_CASE("originator scatter tools reject keys outside the declared buckets") {
  const auto registry = make_registry();
  const auto* group_by = registry.find("group_by");

  auto elements = make_elements();
  // Корзин объявлено меньше, чем встречается ключей: молча ронять элементы в нулевую корзину нельзя.
  auto offsets = make_buffer("offsets", "ui1", 5);
  auto indices = make_buffer("indices", "ui1", count);

  const originator::parameters params;
  const std::vector<originator::field_ref> inputs{readable(elements, "key")};
  const std::vector<originator::field_ref> outputs{writable(offsets, "v"), writable(indices, "v")};

  CHECK_THROWS_AS(originator::dispatch(*group_by, inputs, outputs, params, 1, 0, count, "grouping", nullptr),
                  std::runtime_error);

  // Слишком маленький буфер индексов тоже громкая ошибка, а не порча памяти.
  auto full_offsets = make_buffer("offsets", "ui1", bucket_count + 1);
  auto tiny = make_buffer("indices", "ui1", 16);
  const std::vector<originator::field_ref> tiny_out{writable(full_offsets, "v"), writable(tiny, "v")};
  CHECK_THROWS_AS(originator::dispatch(*group_by, inputs, tiny_out, params, 1, 0, count, "grouping", nullptr),
                  std::runtime_error);
}

TEST_CASE("originator refuses a global scatter while a chunk is being generated") {
  const auto registry = make_registry();
  const auto* group_by = registry.find("group_by");
  const auto* fill = registry.find("fill");
  REQUIRE(group_by != nullptr);
  REQUIRE(fill != nullptr);

  CHECK(originator::parse_key_support("chunk_local") == originator::key_support::chunk_local);
  CHECK(originator::parse_key_support("global") == originator::key_support::global);
  CHECK(originator::parse_key_support("whatever") == originator::key_support::count);
  CHECK(originator::to_string(originator::key_support::chunk_local) == "chunk_local");

  // Группа, собирающая элементы со всей карты, не заканчивается ни одним чанком: результат зависел
  // бы от того, какие чанки успели посчитаться. Это отклоняется до исполнения.
  const auto refused = originator::check_key_support(*group_by, originator::key_support::global, true, "regions");
  CHECK_FALSE(refused.allowed);
  CHECK(refused.message.find("coarse world pass") != std::string::npos);

  // Объявленная чанк-локальная группа разрешена: её чанк заканчивает сам.
  CHECK(originator::check_key_support(*group_by, originator::key_support::chunk_local, true, "regions").allowed);

  // Без чанкования носитель ключа ни на что не влияет.
  CHECK(originator::check_key_support(*group_by, originator::key_support::global, false, "regions").allowed);

  // Для не-scatter апертур проверка не применяется вообще.
  CHECK(originator::check_key_support(*fill, originator::key_support::global, true, "terrain").allowed);
}
