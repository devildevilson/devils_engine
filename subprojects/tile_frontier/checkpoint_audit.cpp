// PRE-02 checkpoint audit over the real tile_frontier actor slice.
//
// The executable measures full canonical write, hash, compression and load, then treats the existing
// component blocks as immutable sections and compares whole-section, 4 KiB page and explicit
// dirty/version reuse. Every reconstructed payload is fed through load_actor_checkpoint and must
// round-trip byte-identically; section-boundary failure injection verifies transactional replacement.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <devils_engine/aesthetics/serialization.h>
#include <devils_engine/aesthetics/sink.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/compression.h>
#include <devils_engine/utils/sha256cpp.h>
#include <devils_engine/utils/timeline.h>
#include <devils_engine/utils/type_traits.h>
#include <spdlog/spdlog.h>

#include "core/actor_simulation.h"
#include "core/actor_checkpoint.h"
#include "test_brain_fixture.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

namespace {

constexpr std::uint64_t document_header_section = UINT64_C(0x100000000);
constexpr std::uint64_t timeline_document_section = UINT64_C(0x100000001);
constexpr std::uint64_t world_frame_section = UINT64_C(0x100000002);
constexpr std::uint64_t world_header_section = UINT64_C(0x100000003);
constexpr std::uint64_t actor_document_section = UINT64_C(0x100000004);
constexpr std::size_t page_size = 4096;

std::atomic_size_t timing_sink = 0;

template <typename T>
[[nodiscard]] std::optional<T> parse_unsigned(const std::string_view value) noexcept {
  T result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
  return result;
}

struct section {
  std::uint64_t id = 0;
  std::string name;
  std::size_t offset = 0;
  std::size_t size = 0;
};

struct checkpoint {
  std::vector<std::byte> packet;
  std::vector<std::byte> raw;
  std::vector<section> sections;
};

struct reuse_result {
  std::size_t total_bytes = 0;
  std::size_t reused_bytes = 0;
  std::size_t total_units = 0;
  std::size_t reused_units = 0;
  std::vector<std::byte> reconstructed;
  std::vector<std::byte> materialized;
};

struct dirty_result : reuse_result {
  std::size_t missed_changed_sections = 0;
  std::size_t redundant_dirty_sections = 0;
};

[[nodiscard]] std::uint32_t read_component_count(const std::span<const std::byte> block) {
  aesthetics::serial::reader reader{block};
  static_cast<void>(reader.u32()); // component hash
  static_cast<void>(reader.u32()); // payload byte length
  const std::uint32_t count = reader.u32();
  if (!reader.ok) utils::error{}("truncated component block");
  return count;
}

[[nodiscard]] std::string component_name(const std::uint32_t hash) {
  const auto& entries = aesthetics::serial::component_registry::table();
  const auto it = std::lower_bound(entries.begin(), entries.end(), hash, [](const auto& entry, const auto value) {
    return entry.hash < value;
  });
  if (it == entries.end() || it->hash != hash) {
    return "unknown." + std::to_string(hash);
  }
  return std::string(it->name);
}

void parse_world_sections(const std::span<const std::byte> raw,
                          const std::size_t base, const std::size_t size,
                          std::vector<section>& result) {
  aesthetics::serial::reader reader{raw.subspan(base, size)};
  if (reader.u32() != aesthetics::serial::snapshot_magic)
    utils::error{}("checkpoint world section has invalid magic");
  static_cast<void>(reader.u32());
  static_cast<void>(reader.u64());
  const std::uint64_t removed_count = reader.u64();
  if (removed_count > size / sizeof(aesthetics::entityid_t))
    utils::error{}("checkpoint world section has invalid removed-entity count");
  for (std::uint64_t i = 0; i < removed_count; ++i) static_cast<void>(reader.u32());
  const std::uint32_t block_count = reader.u32();
  if (!reader.ok) utils::error{}("checkpoint world header is truncated");
  result.push_back(section{world_header_section, "world.header", base, reader.pos});
  for (std::uint32_t i = 0; i < block_count; ++i) {
    const std::size_t start = reader.pos;
    const std::uint32_t hash = reader.u32();
    const std::uint32_t payload_size = reader.u32();
    reader.skip(payload_size);
    if (!reader.ok) utils::error{}("checkpoint component block is truncated");
    result.push_back(section{hash, component_name(hash), base + start, reader.pos - start});
  }
  if (reader.pos != size) utils::error{}("checkpoint world section has trailing bytes");
}

[[nodiscard]] std::vector<section> parse_sections(const std::span<const std::byte> raw) {
  utils::serial::reader reader{raw};
  if (reader.u32() != UINT32_C(0x4e535430)) utils::error{}("checkpoint document has invalid magic");
  static_cast<void>(reader.u32());
  static_cast<void>(reader.u32());
  const std::uint32_t count = reader.u32();
  if (!reader.ok) utils::error{}("checkpoint document header is truncated");

  std::vector<section> result;
  result.reserve(aesthetics::serial::component_registry::table().size() + 5);
  result.push_back(section{document_header_section, "checkpoint.header", 0, reader.pos});
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t frame_start = reader.pos;
    const std::uint32_t id = reader.u32();
    static_cast<void>(reader.u32());
    const std::uint64_t payload_size = reader.u64();
    if (!reader.ok || payload_size > raw.size() - reader.pos)
      utils::error{}("checkpoint section is truncated");
    const std::size_t payload_start = reader.pos;
    reader.skip(std::size_t(payload_size));
    if (id == tf::checkpoint_world_section) {
      result.push_back(section{world_frame_section, "world.section-header", frame_start,
                               payload_start - frame_start});
      parse_world_sections(raw, payload_start, std::size_t(payload_size), result);
    } else {
      const auto stable_id = id == tf::checkpoint_timeline_section
                               ? timeline_document_section
                               : actor_document_section;
      const char* name = id == tf::checkpoint_timeline_section
                           ? "timeline"
                           : "actor.causal";
      result.push_back(section{stable_id, name, frame_start, reader.pos - frame_start});
    }
  }
  if (reader.pos != raw.size()) utils::error{}("checkpoint document has trailing bytes");
  return result;
}

[[nodiscard]] checkpoint capture(const tf::actor_world_slice& slice,
                                 const utils::timelines& clocks) {
  checkpoint result;
  tf::actor_checkpoint_buffers buffers;
  if (!tf::write_actor_checkpoint(slice, clocks, buffers))
    utils::error{}("could not write actor checkpoint");
  result.raw = std::move(buffers.document);
  result.packet = aesthetics::serial::seal(result.raw, aesthetics::serial::network_policy);
  result.sections = parse_sections(result.raw);
  return result;
}

[[nodiscard]] const section* find_section(const checkpoint& value, const std::uint64_t id) {
  const auto it = std::find_if(value.sections.begin(), value.sections.end(), [id](const section& entry) {
    return entry.id == id;
  });
  return it != value.sections.end() ? &*it : nullptr;
}

[[nodiscard]] std::span<const std::byte> bytes_of(const checkpoint& owner, const section& value) {
  return std::span<const std::byte>(owner.raw).subspan(value.offset, value.size);
}

[[nodiscard]] bool equal_bytes(const std::span<const std::byte> a, const std::span<const std::byte> b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

[[nodiscard]] std::uint64_t hash_bytes(const std::span<const std::byte> bytes) {
  return utils::murmur_hash64A(std::string_view(
    reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

[[nodiscard]] bool equal_hashed(const std::span<const std::byte> a, const std::span<const std::byte> b) {
  return a.size() == b.size() && hash_bytes(a) == hash_bytes(b) && equal_bytes(a, b);
}

void append(std::vector<std::byte>& output, const std::span<const std::byte> bytes) {
  output.insert(output.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] reuse_result reuse_whole_sections(const checkpoint& baseline, const checkpoint& current) {
  reuse_result result;
  result.total_bytes = current.raw.size();
  result.total_units = current.sections.size();
  result.reconstructed.reserve(current.raw.size());

  for (const section& cur : current.sections) {
    const auto current_bytes = bytes_of(current, cur);
    const section* old = find_section(baseline, cur.id);
    if (old != nullptr && equal_hashed(bytes_of(baseline, *old), current_bytes)) {
      append(result.reconstructed, bytes_of(baseline, *old));
      result.reused_bytes += cur.size;
      result.reused_units += 1;
    } else {
      append(result.reconstructed, current_bytes);
      append(result.materialized, current_bytes);
    }
  }
  return result;
}

[[nodiscard]] reuse_result reuse_pages(const checkpoint& baseline, const checkpoint& current) {
  reuse_result result;
  result.total_bytes = current.raw.size();
  result.reconstructed.reserve(current.raw.size());

  for (const section& cur : current.sections) {
    const auto current_bytes = bytes_of(current, cur);
    const section* old = find_section(baseline, cur.id);
    const auto old_bytes = old != nullptr ? bytes_of(baseline, *old) : std::span<const std::byte>{};

    for (std::size_t offset = 0; offset < current_bytes.size(); offset += page_size) {
      const std::size_t count = std::min(page_size, current_bytes.size() - offset);
      const auto current_page = current_bytes.subspan(offset, count);
      result.total_units += 1;
      if (offset + count <= old_bytes.size()) {
        const auto old_page = old_bytes.subspan(offset, count);
        if (equal_hashed(old_page, current_page)) {
          append(result.reconstructed, old_page);
          result.reused_bytes += count;
          result.reused_units += 1;
          continue;
        }
      }
      append(result.reconstructed, current_page);
      append(result.materialized, current_page);
    }
  }
  return result;
}

[[nodiscard]] dirty_result reuse_declared_dirty(
  const checkpoint& baseline,
  const checkpoint& current,
  const std::span<const std::uint64_t> dirty_sections) {
  dirty_result result;
  result.total_bytes = current.raw.size();
  result.total_units = current.sections.size();
  result.reconstructed.reserve(current.raw.size());

  for (const section& cur : current.sections) {
    const section* old = find_section(baseline, cur.id);
    const bool marked = std::find(dirty_sections.begin(), dirty_sections.end(), cur.id) != dirty_sections.end();
    const bool changed = old == nullptr || !equal_bytes(bytes_of(baseline, *old), bytes_of(current, cur));
    result.missed_changed_sections += changed && !marked ? 1 : 0;
    result.redundant_dirty_sections += !changed && marked ? 1 : 0;

    if (!marked && old != nullptr) {
      append(result.reconstructed, bytes_of(baseline, *old));
      result.reused_bytes += cur.size;
      result.reused_units += 1;
    } else {
      append(result.reconstructed, bytes_of(current, cur));
      append(result.materialized, bytes_of(current, cur));
    }
  }
  return result;
}

template <typename T>
[[nodiscard]] constexpr std::uint64_t component_section_id() {
  return utils::murmur_hash3_32(utils::type_name<T>());
}

[[nodiscard]] double percent(const std::size_t part, const std::size_t total) {
  return total == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(total);
}

template <typename Function>
[[nodiscard]] double average_microseconds(const std::size_t iterations, Function&& function) {
  std::size_t sink = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    sink ^= static_cast<std::size_t>(function());
  }
  const auto end = std::chrono::steady_clock::now();
  timing_sink.fetch_xor(sink, std::memory_order_relaxed);
  return std::chrono::duration<double, std::micro>(end - begin).count() / static_cast<double>(iterations);
}

void validate_reconstruction(
  const reuse_result& result,
  const checkpoint& expected,
  const tf::brain_config& brains) {
  if (result.reconstructed != expected.raw) {
    utils::error{}("incremental reconstruction does not equal the canonical payload");
  }
  tf::actor_world_slice loaded;
  utils::timelines clocks;
  const auto status = tf::load_actor_checkpoint(loaded, clocks, result.reconstructed, brains);
  if (!status.loaded() || capture(loaded, clocks).raw != expected.raw)
    utils::error{}("reconstructed payload does not load through the checkpoint schema");
}

void print_reuse(const std::string_view name, const reuse_result& result) {
  std::vector<std::uint8_t> changed_bytes;
  changed_bytes.reserve(result.materialized.size());
  for (const std::byte value : result.materialized)
    changed_bytes.push_back(std::to_integer<std::uint8_t>(value));
  const std::size_t compressed_size = changed_bytes.empty()
                                        ? 0
                                        : utils::compress(changed_bytes, utils::compression_level::fast).size();
  const std::size_t changed_units = result.total_units - result.reused_units;
  // This is deliberately an estimate, not a proposed wire format: one common 32-byte header and an
  // optimistic 24-byte locator/hash/length record for every materialized unit.
  const std::size_t estimated_delta_size = changed_units == 0 ? 0 : compressed_size + 32 + changed_units * 24;
  std::cout << "    " << std::left << std::setw(22) << name
            << std::right << std::setw(8) << result.reused_units << '/' << std::setw(4) << result.total_units
            << " units, " << std::setw(10) << result.reused_bytes << '/' << std::setw(10) << result.total_bytes
            << " bytes (" << std::fixed << std::setprecision(1)
            << percent(result.reused_bytes, result.total_bytes) << "%), changed raw/zstd/delta-est="
            << result.materialized.size() << '/' << compressed_size << '/' << estimated_delta_size << " bytes\n";
}

void print_section_inventory(const checkpoint& value) {
  std::cout << "  canonical sections:\n";
  for (const section& item : value.sections) {
    std::cout << "    " << std::left << std::setw(46) << item.name
              << std::right << std::setw(10) << item.size << " bytes";
    if (item.id <= UINT32_MAX && item.id != tf::checkpoint_timeline_section &&
        item.id != tf::checkpoint_world_section && item.id != tf::checkpoint_actor_section) {
      std::cout << ", " << read_component_count(bytes_of(value, item)) << " components";
    }
    std::cout << '\n';
  }
}

void compare_scenario(
  const std::string_view name,
  const checkpoint& baseline,
  const checkpoint& current,
  const tf::brain_config& brains) {
  const auto whole = reuse_whole_sections(baseline, current);
  const auto pages = reuse_pages(baseline, current);
  validate_reconstruction(whole, current, brains);
  validate_reconstruction(pages, current, brains);
  std::cout << "  " << name << ": raw=" << current.raw.size() << " bytes\n";
  std::cout << "    changed sections: ";
  bool any_changed = false;
  for (const section& cur : current.sections) {
    const section* old = find_section(baseline, cur.id);
    if (old == nullptr || !equal_hashed(bytes_of(baseline, *old), bytes_of(current, cur))) {
      std::cout << (any_changed ? ", " : "") << cur.name;
      any_changed = true;
    }
  }
  std::cout << (any_changed ? "\n" : "none\n");
  print_reuse("whole sections", whole);
  print_reuse("4 KiB pages", pages);
}

struct fixture_result {
  std::uint32_t entity_count = 0;
  checkpoint base;
  checkpoint local_change;
  checkpoint one_tick;
  checkpoint five_ticks;
  checkpoint twenty_ticks;
  checkpoint structural_change;
  checkpoint structural_remove;
};

[[nodiscard]] fixture_result build_fixture(
  const std::uint32_t entity_count,
  const std::size_t warmup_ticks,
  const tf::brain_config& brains) {
  const glm::vec2 min_bound{0.5f, 0.5f};
  const glm::vec2 max_bound{64.0f, 64.0f};

  thread::atomic_pool pool(4);
  tf::actor_batch batch;
  batch.bind("v2ui1c4v1");
  if (!batch.valid()) utils::error{}("actor batch layout is invalid");

  tf::actor_world_slice source;
  source.init(entity_count, min_bound, max_bound, 4, brains);
  utils::timelines source_clocks(utils::simulation_rate(60));
  for (std::size_t i = 0; i < warmup_ticks; ++i) {
    const auto tick = source_clocks.simulation_now() + utils::simulation_duration{1};
    source.update(tick, source_clocks.advance_simulation(tick), batch, pool);
  }

  fixture_result result;
  result.entity_count = entity_count;
  result.base = capture(source, source_clocks);

  tf::actor_world_slice local;
  utils::timelines local_clocks;
  if (!tf::load_actor_checkpoint(local, local_clocks, result.base.raw, brains).loaded())
    utils::error{}("could not clone local-change fixture");
  bool changed_position = false;
  for (auto [id, position] : local.ecs().view<tf::actor_position>()) {
    static_cast<void>(id);
    position->value.x += 0.125f;
    changed_position = true;
    break;
  }
  if (!changed_position) utils::error{}("local-change fixture has no position");
  result.local_change = capture(local, local_clocks);

  tf::actor_world_slice ticked;
  utils::timelines ticked_clocks(utils::simulation_rate(1));
  if (!tf::load_actor_checkpoint(ticked, ticked_clocks, result.base.raw, brains).loaded())
    utils::error{}("could not clone tick fixture");
  auto update_ticked = [&]() {
    const auto tick = ticked_clocks.simulation_now() + utils::simulation_duration{1};
    ticked.update(tick, ticked_clocks.advance_simulation(tick), batch, pool);
  };
  update_ticked();
  result.one_tick = capture(ticked, ticked_clocks);
  for (std::size_t i = 1; i < 5; ++i)
    update_ticked();
  result.five_ticks = capture(ticked, ticked_clocks);
  for (std::size_t i = 5; i < 20; ++i)
    update_ticked();
  result.twenty_ticks = capture(ticked, ticked_clocks);

  tf::actor_world_slice structural;
  utils::timelines structural_clocks;
  if (!tf::load_actor_checkpoint(structural, structural_clocks, result.base.raw, brains).loaded())
    utils::error{}("could not clone structural fixture");
  static_cast<void>(structural.spawn_prefab("food", glm::vec2{3.0f, 5.0f}));
  result.structural_change = capture(structural, structural_clocks);

  tf::actor_world_slice removed;
  utils::timelines removed_clocks;
  if (!tf::load_actor_checkpoint(removed, removed_clocks, result.base.raw, brains).loaded())
    utils::error{}("could not clone removal fixture");
  aesthetics::entityid_t removed_id = aesthetics::invalid_entityid;
  for (auto [id, food] : removed.ecs().view<tf::food_item>()) {
    static_cast<void>(food);
    removed_id = id;
    break;
  }
  if (removed_id == aesthetics::invalid_entityid) utils::error{}("removal fixture has no food entity");
  removed.ecs().remove_entity(removed_id);
  result.structural_remove = capture(removed, removed_clocks);
  return result;
}

void validate_dirty_prototypes(const fixture_result& fixture, const tf::brain_config& brains) {
  const std::uint64_t position = component_section_id<tf::actor_position>();
  const std::uint64_t visual = component_section_id<tf::actor_visual>();
  const std::uint64_t food = component_section_id<tf::food_item>();
  const std::vector<std::uint64_t> local_dirty{position};
  const std::vector<std::uint64_t> structural_dirty{
    world_frame_section, world_header_section, position, visual, food};

  const auto local = reuse_declared_dirty(fixture.base, fixture.local_change, local_dirty);
  const auto structural = reuse_declared_dirty(fixture.base, fixture.structural_change, structural_dirty);
  const auto removal = reuse_declared_dirty(fixture.base, fixture.structural_remove, structural_dirty);
  validate_reconstruction(local, fixture.local_change, brains);
  validate_reconstruction(structural, fixture.structural_change, brains);
  validate_reconstruction(removal, fixture.structural_remove, brains);
  if (local.missed_changed_sections != 0 || structural.missed_changed_sections != 0 ||
      removal.missed_changed_sections != 0) {
    utils::error{}("explicit dirty prototype missed a changed section");
  }

  std::cout << "  explicit dirty/version prototype:\n";
  print_reuse("one position write", local);
  std::cout << "      missed=" << local.missed_changed_sections
            << ", redundant=" << local.redundant_dirty_sections << '\n';
  print_reuse("one entity spawn", structural);
  std::cout << "      missed=" << structural.missed_changed_sections
            << ", redundant=" << structural.redundant_dirty_sections << '\n';
  print_reuse("one entity removal", removal);
  std::cout << "      missed=" << removal.missed_changed_sections
            << ", redundant=" << removal.redundant_dirty_sections << '\n';
}

void measure_full_checkpoint(
  const fixture_result& fixture,
  const std::size_t iterations,
  const tf::brain_config& brains) {
  tf::actor_world_slice live;
  utils::timelines live_clocks;
  if (!tf::load_actor_checkpoint(live, live_clocks, fixture.base.raw, brains).loaded())
    utils::error{}("could not initialize timing fixture");

  std::vector<std::uint8_t> raw_u8;
  raw_u8.reserve(fixture.base.raw.size());
  for (const std::byte value : fixture.base.raw)
    raw_u8.push_back(std::to_integer<std::uint8_t>(value));

  const auto normal_packet = aesthetics::serial::seal(fixture.base.raw, aesthetics::serial::disk_policy);
  const double ecs_dump_us = average_microseconds(iterations, [&] {
    return aesthetics::serial::dump_world(&live.ecs()).size();
  });
  tf::actor_checkpoint_buffers write_buffers;
  if (!tf::write_actor_checkpoint(live, live_clocks, write_buffers))
    utils::error{}("could not prepare timed checkpoint buffers");
  const double full_save_us = average_microseconds(iterations, [&] {
    if (!tf::write_actor_checkpoint(live, live_clocks, write_buffers))
      utils::error{}("timed checkpoint write failed");
    return write_buffers.document.size();
  });

  const double murmur64_us = average_microseconds(iterations, [&] {
    return hash_bytes(fixture.base.raw);
  });
  const double sha256_us = average_microseconds(iterations, [&] {
    utils::SHA256 hasher;
    hasher.update(fixture.base.raw.data(), fixture.base.raw.size());
    return hasher.finalize().front();
  });
  const double section_hash_us = average_microseconds(iterations, [&] {
    std::uint64_t value = 0;
    for (const section& item : fixture.base.sections)
      value ^= hash_bytes(bytes_of(fixture.base, item));
    return value;
  });
  const double page_hash_us = average_microseconds(iterations, [&] {
    std::uint64_t value = 0;
    for (const section& item : fixture.base.sections) {
      const auto bytes = bytes_of(fixture.base, item);
      for (std::size_t offset = 0; offset < bytes.size(); offset += page_size) {
        value ^= hash_bytes(bytes.subspan(offset, std::min(page_size, bytes.size() - offset)));
      }
    }
    return value;
  });
  const double compress_us = average_microseconds(iterations, [&] {
    return utils::compress(raw_u8, utils::compression_level::fast).size();
  });
  const double seal_us = average_microseconds(iterations, [&] {
    return aesthetics::serial::seal(fixture.base.raw, aesthetics::serial::network_policy).size();
  });
  const double unseal_us = average_microseconds(iterations, [&] {
    std::vector<std::byte> raw;
    if (!aesthetics::serial::unseal(fixture.base.packet, raw)) utils::error{}("timed unseal failed");
    return raw.size();
  });
  const std::size_t load_iterations = std::max<std::size_t>(1, iterations / 4);
  const double load_us = average_microseconds(load_iterations, [&] {
    tf::actor_world_slice loaded;
    utils::timelines clocks;
    if (!tf::load_actor_checkpoint(loaded, clocks, fixture.base.raw, brains).loaded())
      utils::error{}("timed full load failed");
    return loaded.ecs().index_capacity();
  });

  std::cout << "  full checkpoint sizes: raw=" << fixture.base.raw.size()
            << ", zstd-fast container=" << fixture.base.packet.size()
            << " (" << std::fixed << std::setprecision(1)
            << percent(fixture.base.packet.size(), fixture.base.raw.size()) << "%), zstd-normal container="
            << normal_packet.size() << " (" << percent(normal_packet.size(), fixture.base.raw.size()) << "%)\n";
  std::cout << "  average over " << iterations << " iterations: Murmur64=" << std::setprecision(2) << murmur64_us
            << " us, SHA-256=" << sha256_us
            << " us, section hashes=" << section_hash_us << " us, page hashes=" << page_hash_us
            << " us, zstd-fast=" << compress_us << " us, seal(hash+compress+container)=" << seal_us
            << " us, ECS dump=" << ecs_dump_us << " us, full save=" << full_save_us
            << " us, unseal=" << unseal_us << " us, full load=" << load_us
            << " us (" << load_iterations << " iterations)\n";
}

[[nodiscard]] std::size_t transactional_failure_checks(
  const checkpoint& baseline,
  const tf::brain_config& brains) {
  tf::actor_world_slice destination;
  utils::timelines destination_clocks;
  if (!tf::load_actor_checkpoint(destination, destination_clocks, baseline.raw, brains).loaded())
    utils::error{}("could not initialize corruption fixture");
  tf::actor_batch batch;
  batch.bind("v2ui1c4v1");
  thread::atomic_pool pool(1);
  const auto first_tick = destination_clocks.simulation_now() + utils::simulation_duration{1};
  static_cast<void>(destination.update(
    first_tick, destination_clocks.advance_simulation(first_tick), batch, pool));
  const auto before = capture(destination, destination_clocks).raw;

  std::size_t checked = 0;
  auto corrupt = baseline.packet;
  corrupt.back() ^= std::byte{0xff};
  std::vector<std::byte> corrupt_raw;
  if (aesthetics::serial::unseal(corrupt, corrupt_raw))
    utils::error{}("corrupt checkpoint envelope was accepted");
  if (capture(destination, destination_clocks).raw != before)
    utils::error{}("outer-container failure changed live state");
  ++checked;

  // A valid checksum around a truncated raw payload forces the loader through every successfully
  // decoded prefix.  Cut immediately after each state section; the final section instead gets a
  // trailing byte, which exercises the strict end-of-project-payload check.
  for (const section& item : baseline.sections) {
    const std::size_t end = item.offset + item.size;
    std::vector<std::byte> invalid;
    if (end < baseline.raw.size()) {
      invalid.assign(baseline.raw.begin(), baseline.raw.begin() + std::ptrdiff_t(end));
    } else {
      invalid = baseline.raw;
      invalid.push_back(std::byte{0x5a});
    }
    if (tf::load_actor_checkpoint(destination, destination_clocks, invalid, brains).loaded()) {
      utils::error{}("checkpoint failure injection after section '{}' was accepted", item.name);
    }
    if (capture(destination, destination_clocks).raw != before) {
      utils::error{}("checkpoint failure after section '{}' changed live state", item.name);
    }
    ++checked;
  }

  // Correct framing with invalid values exercises validation AFTER a complete world was staged.
  // Corrupt rate in timeline, then the actor budget in the final section.
  for (const auto id : {timeline_document_section, actor_document_section}) {
    const auto* item = find_section(baseline, id);
    if (item == nullptr) utils::error{}("missing causal section in corruption fixture");
    auto invalid = baseline.raw;
    const std::size_t offset = id == timeline_document_section ? item->offset + 16 + 32
                                                     : item->offset + item->size - 8;
    const std::size_t width = id == timeline_document_section ? 4 : 8;
    std::fill_n(invalid.begin() + std::ptrdiff_t(offset), width, std::byte{0});
    if (tf::load_actor_checkpoint(destination, destination_clocks, invalid, brains).loaded())
      utils::error{}("invalid causal section was accepted");
    if (capture(destination, destination_clocks).raw != before)
      utils::error{}("causal validation failure changed live state");
    ++checked;
  }

  // The preserved instance is not merely byte-identical: its lazily constructed systems and their
  // queries remain usable after the whole refusal sequence.
  const auto next_tick = destination_clocks.simulation_now() + utils::simulation_duration{1};
  static_cast<void>(destination.update(
    next_tick, destination_clocks.advance_simulation(next_tick), batch, pool));

  return checked;
}

void run_fixture(
  const std::uint32_t entity_count,
  const std::size_t warmup_ticks,
  const std::size_t timing_iterations,
  const tf::brain_config& brains) {
  const fixture_result fixture = build_fixture(entity_count, warmup_ticks, brains);
  std::cout << "\nfixture: requested actors=" << entity_count << ", warmup ticks=" << warmup_ticks
            << ", canonical sections=" << fixture.base.sections.size() << '\n';
  print_section_inventory(fixture.base);
  measure_full_checkpoint(fixture, timing_iterations, brains);
  compare_scenario("identical", fixture.base, fixture.base, brains);
  compare_scenario("one position write", fixture.base, fixture.local_change, brains);
  compare_scenario("one simulation tick", fixture.base, fixture.one_tick, brains);
  compare_scenario("five simulation ticks", fixture.base, fixture.five_ticks, brains);
  compare_scenario("twenty simulation ticks", fixture.base, fixture.twenty_ticks, brains);
  compare_scenario("one entity spawn", fixture.base, fixture.structural_change, brains);
  compare_scenario("one entity removal", fixture.base, fixture.structural_remove, brains);
  validate_dirty_prototypes(fixture, brains);

  const std::size_t transactional_checks = transactional_failure_checks(fixture.base, brains);
  std::cout << "  transactional load failures preserve destination: yes ("
            << transactional_checks << " injected failures)\n";
}

} // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::err);
  const auto small = argc > 1 ? parse_unsigned<std::uint32_t>(argv[1])
                              : std::optional<std::uint32_t>{512u};
  const auto large = argc > 2 ? parse_unsigned<std::uint32_t>(argv[2])
                              : std::optional<std::uint32_t>{8192u};
  const auto warmup = argc > 3 ? parse_unsigned<std::size_t>(argv[3])
                               : std::optional<std::size_t>{20u};
  if (!small || !large || !warmup || *small == 0 || *large < *small || *warmup == 0) {
    std::cerr << "usage: tile_frontier_checkpoint_audit [small>0] [large>=small] [warmup>0]\n";
    return 2;
  }

  test_brain_fixture brains(TILE_FRONTIER_SOURCE_RESOURCE_ROOT);
  std::cout << "PRE-02 tile_frontier checkpoint audit (4 KiB pages)\n";
  run_fixture(*small, *warmup, 12, brains.config());
  run_fixture(*large, *warmup, 4, brains.config());
  std::cout << "\nPRE-02 AUDIT OK: full and reconstructed checkpoints load byte-identically\n";
  return 0;
}
