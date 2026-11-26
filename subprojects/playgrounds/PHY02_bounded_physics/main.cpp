#include "world.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <devils_engine/playground/cli.h>
#include <devils_engine/utils/hash.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

using namespace devils_engine;

namespace devils_engine::phy02 {
namespace {

struct shapes {
  JPH::ShapeRefC floor = new JPH::BoxShape({24, 0.5f, 24});
  JPH::ShapeRefC box = new JPH::BoxShape({0.45f, 0.45f, 0.45f});
  JPH::ShapeRefC ball = new JPH::SphereShape(0.45f);
  JPH::ShapeRefC probe = new JPH::SphereShape(1.7f);
  JPH::ShapeRefC compound;

  shapes() {
    JPH::StaticCompoundShapeSettings settings;
    settings.AddShape({-1, 0, 0}, JPH::Quat::sIdentity(), box.GetPtr());
    settings.AddShape({1, 0, 0}, JPH::Quat::sIdentity(), box.GetPtr());
    const auto cooked = settings.Create();
    if (cooked.HasError()) utils::error{}("PHY02 compound cooking: {}", cooked.GetError().c_str());
    compound = cooked.Get();
  }
};

body_handle add(world& physics, const JPH::Shape* shape, JPH::RVec3Arg p,
                JPH::EMotionType motion, bool allow_sleep = true) {
  const auto result = physics.queue(shape, p, motion, allow_sleep);
  if (result.status != world::command_status::ok) {
    utils::error{}("PHY02 queue body at ({}, {}, {}): status {}", p.GetX(), p.GetY(), p.GetZ(), int(result.status));
  }
  return result.handle;
}

void publish(world& physics) {
  const auto result = physics.publish();
  if (result != world::command_status::ok) utils::error{}("PHY02 body publication: status {}", int(result));
}

step_result advance(world& physics, uint32_t tick) {
  const auto result = physics.step();
  if (result.solver != JPH::EPhysicsUpdateError::None) {
    utils::error{}("PHY02 solver at tick {}: Update flags {}, limits {} body pairs / {} contact constraints",
                   tick, uint32_t(result.solver), physics.capacity().solver_pairs, physics.capacity().solver_contacts);
  }
  switch (result.contacts) {
    case contact_status::ok: break;
    case contact_status::event_capacity:
      utils::error{}("PHY02 event capacity at tick {}: {} callbacks offered, limit {}",
              tick, physics.event_peak(), physics.capacity().events);
    case contact_status::pair_capacity:
      utils::error{}("PHY02 pair capacity at tick {}: another pair offered at limit {}", tick, physics.capacity().pairs);
    case contact_status::unresolved_removal:
      utils::error{}("PHY02 removed contact has no active pair at tick {}", tick);
  }
  return result;
}

struct settings {
  uint32_t bodies = 96;
  uint32_t steps = 120;
  uint32_t workers = 4;
  bool dense = true;
  bool shared_jobs = false;
  pair_storage pairs = pair_storage::hash;
  event_storage events = event_storage::lanes;
  lookup_mode lookup = lookup_mode::index;
};

constexpr uint32_t warmup_ticks = 8;
constexpr size_t batch_queries = 12;
constexpr size_t hits_per_query = 128;
constexpr size_t candidates_per_query = 1024;

class query_batch {
public:
  query_batch() = default;

  explicit query_batch(const shapes& cooked) {
    for (size_t i = 0; i < batch_queries; ++i) {
      const float x = (float(i % 8) - 3.5f) * 1.04f;
      const float z = 0.17f * float(i % 3);
      const auto kind = i % 2 == 0 ? query_kind::ray : query_kind::overlap;
      const auto origin = kind == query_kind::ray ? JPH::RVec3(x, 85, z) : JPH::RVec3(x, 1.1f, z);
      if (!push({kind, origin, {0, -86, 0}, cooked.probe.GetPtr()})) {
        utils::error{}("PHY02 fixture: query {} exceeded the batch capacity {}", i, commands_.size());
      }
    }
  }

  bool push(query command) {
    if (count_ == commands_.size()) return false;
    commands_[count_++] = command;
    return true;
  }

  size_t size() const noexcept { return count_; }

  void run(const world& physics, thread::atomic_pool& pool, lookup_mode lookup) {
    pool.distribute(count_, [this, &physics, lookup](size_t start, size_t count) {
      const auto executing = active_jobs_.fetch_add(1, std::memory_order_relaxed) + 1;
      auto peak = job_peak_.load(std::memory_order_relaxed);
      while (peak < executing && !job_peak_.compare_exchange_weak(peak, executing, std::memory_order_relaxed)) {}
      for (size_t i = start; i < start + count; ++i) {
        auto& row = rows_[i];
        row.result = physics.run(commands_[i], row.hits, candidates_per_query, lookup);
        row.unique_count = row.result.status == query_status::ok
          ? world::objects(row.result.hits(), row.objects) : 0;
      }
      active_jobs_.fetch_sub(1, std::memory_order_relaxed);
    });
    pool.wait();
    for (const auto& row : std::span(rows_).first(count_)) {
      if (row.result.status != query_status::ok) {
        utils::error{}("PHY02 query: status {}, candidates {} / {}, hits {} / {}",
                       int(row.result.status), row.result.candidates, candidates_per_query, row.result.offered, hits_per_query);
      }
      candidate_peak_ = std::max(candidate_peak_, row.result.candidates);
      hit_peak_ = std::max(hit_peak_, row.result.offered);
      candidate_total_ += row.result.candidates;
      hit_total_ += row.result.count;
    }
  }

  void append(std::vector<uint64_t>& trace) const {
    for (const auto& row : std::span(rows_).first(count_)) {
      trace.push_back(row.result.count);
      for (const auto& h : row.result.hits()) {
        trace.push_back(h.key);
        trace.push_back(h.subshape);
        trace.push_back(std::bit_cast<uint32_t>(h.fraction));
      }
      trace.push_back(row.unique_count);
      trace.insert(trace.end(), row.objects.begin(), row.objects.begin() + row.unique_count);
    }
  }

  size_t candidate_peak() const noexcept { return candidate_peak_; }
  size_t hit_peak() const noexcept { return hit_peak_; }
  size_t candidate_total() const noexcept { return candidate_total_; }
  size_t hit_total() const noexcept { return hit_total_; }
  size_t job_peak() const noexcept { return job_peak_.load(std::memory_order_relaxed); }

private:
  struct row {
    std::array<hit, hits_per_query> hits;
    std::array<uint64_t, hits_per_query> objects;
    query_result result{};
    size_t unique_count = 0;
  };
  std::array<query, batch_queries> commands_;
  std::array<row, batch_queries> rows_;
  size_t count_ = 0;
  size_t candidate_peak_ = 0;
  size_t hit_peak_ = 0;
  size_t candidate_total_ = 0;
  size_t hit_total_ = 0;
  std::atomic<size_t> active_jobs_{0};
  std::atomic<size_t> job_peak_{0};
};

void report_allocations(const char* name, const allocation_sample& value) {
  std::printf("  %s allocations engine=%" PRIu64 "/%" PRIu64 " B jolt=%" PRIu64 "/%" PRIu64 " B\n",
    name, value.engine.calls, value.engine.bytes, value.jolt.calls, value.jolt.bytes);
}

void add_allocations(allocation_sample& total, const allocation_sample& sample) {
  total.engine.calls += sample.engine.calls;
  total.engine.bytes += sample.engine.bytes;
  total.jolt.calls += sample.jolt.calls;
  total.jolt.bytes += sample.jolt.bytes;
}

struct run_report {
  std::vector<uint64_t> trace;
  std::vector<size_t> tick_ends;
};

run_report run(const shapes& cooked, settings config, bool record) {
  const auto initial_allocations = allocation_totals();
  // Reserve one executor for the independent load in both variants.
  thread::atomic_pool pool(config.shared_jobs ? config.workers + 1 : 1);
  limits cap;
  cap.bodies = config.bodies + 8;
  cap.commands = cap.bodies;
  cap.pairs = std::max(size_t{512}, cap.bodies * 12);
  cap.events = cap.pairs * 2;
  cap.solver_pairs = uint32_t(cap.pairs * 2);
  cap.solver_contacts = uint32_t(cap.pairs);
  world physics(cap, config.pairs, config.events, config.workers, config.shared_jobs, pool);

  const auto prepare_allocations = allocation_delta(initial_allocations);
  const auto before_publish = allocation_totals();
  const auto publish_started = std::chrono::steady_clock::now();
  add(physics, cooked.floor.GetPtr(), {0, -0.5f, 0}, JPH::EMotionType::Static);
  body_handle first;
  for (uint32_t i = 0; i < config.bodies; ++i) {
    const float x = config.dense ? (float(i % 8) - 3.5f) * 1.04f : (float(i % 16) - 7.5f) * 2.45f;
    const float y = config.dense ? 0.5f + float(i / 8) * 0.99f : 0.5f + float(i / 256) * 1.05f;
    const float z = config.dense ? (float(i % 3) - 1) * 0.07f : (float(i / 16 % 16) - 7.5f) * 2.45f;
    const auto* shape = i % 13 == 0 ? cooked.compound.GetPtr()
                          : i % 2 == 0 ? cooked.box.GetPtr() : cooked.ball.GetPtr();
    const auto handle = add(physics, shape, {x, y, z}, JPH::EMotionType::Dynamic, false);
    if (i == 0) first = handle;
  }
  publish(physics);
  physics.impulse(first, {0.0125f, 0, 0.025f});
  physics.optimize();
  const auto publish_us = micros(std::chrono::steady_clock::now() - publish_started);
  const auto publication_allocations = allocation_delta(before_publish);

  query_batch queries(cooked);
  run_report recorded;
  if (record) {
    recorded.trace.reserve(size_t(config.steps) * (size_t(config.bodies + 1) * 15 + 128));
    recorded.tick_ends.reserve(config.steps);
  }
  std::vector<double> durations;
  durations.reserve(config.steps - warmup_ticks);
  double update_us = 0, gather_us = 0, seal_us = 0, query_us = 0;
  std::clock_t cpu = 0;
  uint64_t background_checksum = 0;
  uint64_t background_jobs = 0;
  allocation_sample warm{}, hot_update{}, hot_queries{};
  for (uint32_t tick = 1; tick <= config.steps; ++tick) {
    const auto before = allocation_totals();
    const auto started = std::chrono::steady_clock::now();
    const auto cpu_start = std::clock();
    pool.submit([tick, &background_checksum] {
      uint64_t value = tick;
      for (uint32_t i = 0; i < 2048; ++i) {
        value = utils::hash_combine(value, i);
      }
      background_checksum ^= value;
    });
    ++background_jobs;
    const auto step = advance(physics, tick);
    // In the stock variant this drains the independent engine task. In the
    // shared variant finish() already drained the queued job references.
    pool.wait();
    const auto before_queries = allocation_totals();
    const auto query_start = std::chrono::steady_clock::now();
    queries.run(physics, pool, config.lookup);
    const auto ended = std::chrono::steady_clock::now();
    const auto cpu_end = std::clock();
    const auto after = allocation_totals();
    if (tick <= warmup_ticks) {
      add_allocations(warm, allocation_delta(before, after));
    } else {
      add_allocations(hot_update, allocation_delta(before, before_queries));
      add_allocations(hot_queries, allocation_delta(before_queries, after));
      durations.push_back(micros(ended - started));
      update_us += step.update_us;
      gather_us += step.gather_us;
      seal_us += step.seal_us;
      query_us += micros(ended - query_start);
      cpu += cpu_end - cpu_start;
    }
    if (record) {
      physics.append_state(recorded.trace);
      const auto events = physics.contacts();
      recorded.trace.push_back(events.size());
      for (const auto& event : events) {
        recorded.trace.push_back(event.pair.first);
        recorded.trace.push_back(event.pair.second);
        recorded.trace.push_back(event.pair.first_shape);
        recorded.trace.push_back(event.pair.second_shape);
        recorded.trace.push_back(uint32_t(event.phase));
      }
      queries.append(recorded.trace);
      recorded.tick_ends.push_back(recorded.trace.size());
    }
  }

  utils::deterministic_sort(durations.begin(), durations.end(), std::less{});
  const auto quantile = [&](size_t percent) {
    return durations[(durations.size() * percent + 99) / 100 - 1];
  };
  const double n = double(durations.size());
  std::printf("%s %s workers=%" PRIu32 " pairs=%s events=%s lookup=%s bodies=%" PRIu32 " steps=%" PRIu32 "\n",
    config.dense ? "dense" : "sparse", config.shared_jobs ? "shared" : "stock", config.workers,
    config.pairs == pair_storage::hash ? "hash" : "linear",
    config.events == event_storage::lanes ? "lanes" : "shared",
    config.lookup == lookup_mode::index ? "index" : config.lookup == lookup_mode::linear ? "linear" : "user-data",
    config.bodies, config.steps);
  std::printf("  peaks bodies=%zu/%zu commands=%zu/%zu pairs=%zu/%zu callbacks=%zu/%zu scratch=%zu/%zu B\n",
    physics.body_peak(), cap.bodies, physics.command_peak(), cap.commands, physics.pair_peak(), cap.pairs,
    physics.event_peak(), cap.events, physics.scratch_peak(), cap.scratch);
  std::printf("  reserves world_buffers=%zu B pair_cache=%zu B query_batch=%zu B shared_queue_peak=%zu\n",
    physics.reserved_bytes(), physics.pair_bytes(), sizeof(queries), physics.queue_peak());
  std::printf("  queries=%zu batch=%zu/%zu candidates=%zu peak=%zu/%zu hits=%zu offered_peak=%zu/%zu query_jobs=%zu/%zu background=%" PRIu64 "/%" PRIx64 "\n",
    queries.size() * config.steps, queries.size(), batch_queries, queries.candidate_total(),
    queries.candidate_peak(), candidates_per_query, queries.hit_total(), queries.hit_peak(), hits_per_query,
    queries.job_peak(), pool.size(), background_jobs, background_checksum);
  std::printf("  microseconds mean update=%.2f gather=%.2f seal=%.2f query=%.2f publish=%.2f | tick p50=%.2f p95=%.2f p99=%.2f CPU/tick=%.2f\n",
    update_us / n, gather_us / n, seal_us / n, query_us / n, publish_us,
    quantile(50), quantile(95), quantile(99), double(cpu) * 1.e6 / (double(CLOCKS_PER_SEC) * n));
  report_allocations("prepare", prepare_allocations);
  report_allocations("body publication", publication_allocations);
  report_allocations("warmup", warm);
  report_allocations("steady-state step and contacts", hot_update);
  report_allocations("steady-state queries", hot_queries);
  auto hot = hot_update;
  add_allocations(hot, hot_queries);
  if (hot.engine.calls != 0) {
    utils::error{}("PHY02 steady-state: engine allocated {} times ({} bytes)", hot.engine.calls, hot.engine.bytes);
  }
  if (!physics.scratch_empty()) utils::error{}("PHY02 steady-state: solver did not return all step scratch");
  if (physics.event_peak() == 0 || queries.hit_total() == 0) {
    utils::error{}("PHY02 simulation fixture: expected contacts and query hits, got callback peak {} and {} hits",
                   physics.event_peak(), queries.hit_total());
  }
  return recorded;
}

void verify_queries(const shapes& cooked) {
  thread::atomic_pool pool(1);
  limits cap;
  cap.bodies = 3;
  cap.commands = 2;
  world physics(cap, pair_storage::hash, event_storage::lanes, 0, false, pool);
  const auto compound = add(physics, cooked.compound.GetPtr(), {0, 5, 0}, JPH::EMotionType::Static);
  const auto twin_a = add(physics, cooked.ball.GetPtr(), {4, 5, 0}, JPH::EMotionType::Static);
  const auto command_refusal = physics.queue(cooked.ball.GetPtr(), {4, 5, 0}, JPH::EMotionType::Static);
  if (command_refusal.status != world::command_status::command_capacity) {
    utils::error{}("PHY02 commands: third command at capacity {} returned status {}", cap.commands, int(command_refusal.status));
  }
  const query ray{query_kind::ray, {-6, 5, 0}, {12, 0, 0}};
  query_batch batch;
  for (size_t i = 0; i < batch_queries; ++i) {
    if (!batch.push(ray)) utils::error{}("PHY02 query batch: rejected query {} before capacity {}", i, batch_queries);
  }
  if (batch.push(ray) || batch.size() != batch_queries) {
    utils::error{}("PHY02 query batch: size {} after pushing past capacity {}", batch.size(), batch_queries);
  }
  std::array<hit, 8> scratch;
  const auto pending = physics.run(ray, scratch, 8, lookup_mode::index);
  if (pending.count != 0) utils::error{}("PHY02 body publication: ray returned {} hits before publish", pending.count);
  publish(physics);
  batch.run(physics, pool, lookup_mode::index);
  if (batch.hit_total() != 3 * batch_queries) {
    utils::error{}("PHY02 query batch: {} hits after overflow refusal, expected {}", batch.hit_total(), 3 * batch_queries);
  }
  const auto twin_b = add(physics, cooked.ball.GetPtr(), {4, 5, 0}, JPH::EMotionType::Static);
  publish(physics);
  const auto body_refusal = physics.queue(cooked.box.GetPtr(), {1, 0, 0}, JPH::EMotionType::Static);
  if (body_refusal.status != world::command_status::body_capacity) {
    utils::error{}("PHY02 bodies: creation at capacity {} returned status {}", cap.bodies, int(body_refusal.status));
  }
  const JPH::AABox bounds(JPH::Vec3(-2, 4, -1), JPH::Vec3(5, 6, 1));
  const auto coarse = physics.candidates(bounds, std::span(scratch).first(3));
  if (coarse.status != query_status::ok || coarse.count != 3) {
    utils::error{}("PHY02 coarse query: status {}, count {}, expected success with 3 candidates", int(coarse.status), coarse.count);
  }
  if (coarse.hits()[0].key != compound.key || coarse.hits()[1].key != twin_a.key || coarse.hits()[2].key != twin_b.key) {
    utils::error{}("PHY02 coarse query: unexpected ordered keys [{}, {}, {}], expected [{}, {}, {}]",
                   coarse.hits()[0].key, coarse.hits()[1].key, coarse.hits()[2].key, compound.key, twin_a.key, twin_b.key);
  }
  const auto coarse_refusal = physics.candidates(bounds, std::span(scratch).first(2));
  if (coarse_refusal.status != query_status::candidate_capacity || !coarse_refusal.hits().empty()) {
    utils::error{}("PHY02 coarse query: capacity 2 returned status {} and {} candidate keys",
                   int(coarse_refusal.status), coarse_refusal.hits().size());
  }
  const std::array owners{compound.key, compound.key, twin_a.key, twin_b.key};
  const std::array centers{-1.0f, 1.0f, 4.0f, 4.0f};
  // Full enumeration for this collinear fixture. Rays hit a central section
  // of each analytic box/sphere, where the entry is (center - half extent).
  for (auto mode : {lookup_mode::linear, lookup_mode::index, lookup_mode::user_data}) {
    const auto found = physics.run(ray, std::span(scratch).first(4), 8, mode);
    if (found.status != query_status::ok || found.count != owners.size()) {
      utils::error{}("PHY02 ray: lookup {} returned status {} and {} hits, expected {} hits",
                     int(mode), int(found.status), found.count, owners.size());
    }
    for (size_t i = 0; i < owners.size(); ++i) {
      const auto& hit = found.hits()[i];
      if (hit.key != owners[i]) {
        utils::error{}("PHY02 ray: lookup {} hit {} has key {}, expected {}", int(mode), i, hit.key, owners[i]);
      }
      const float expected_fraction = (centers[i] - 0.45f + 6.0f) / 12.0f;
      if (!(std::abs(hit.fraction - expected_fraction) < 0.00001f)) {
        utils::error{}("PHY02 ray: hit {} fraction {} differs from analytic fraction {}", i, hit.fraction, expected_fraction);
      }
    }
    if (found.hits()[0].subshape == found.hits()[1].subshape) {
      utils::error{}("PHY02 ray: both hits on compound {} have subshape {}", compound.key, found.hits()[0].subshape);
    }
    std::array<uint64_t, 8> objects;
    const auto unique = world::objects(found.hits(), objects);
    if (unique != 3) utils::error{}("PHY02 each_object: {} targets from {} hits, expected 3", unique, found.count);
  }
  for (const size_t capacity : {0, 1, 3}) {
    const auto refused = physics.run(ray, std::span(scratch).first(capacity), 8, lookup_mode::index);
    if (refused.status != query_status::hit_capacity || !refused.hits().empty()) {
      utils::error{}("PHY02 ray: hit capacity {} returned status {} and {} hits",
                     capacity, int(refused.status), refused.hits().size());
    }
  }
  const auto candidates = physics.run(ray, scratch, 0, lookup_mode::index);
  if (candidates.status != query_status::candidate_capacity || !candidates.hits().empty()) {
    utils::error{}("PHY02 ray: candidate capacity 0 returned status {} and {} hits",
                   int(candidates.status), candidates.hits().size());
  }
  const query miss{query_kind::ray, {-6, 7, 0}, {12, 0, 0}};
  const auto empty = physics.run(miss, {}, 8, lookup_mode::index);
  if (empty.status != query_status::ok || empty.count != 0) {
    utils::error{}("PHY02 ray miss: hit capacity 0 returned status {} and {} hits", int(empty.status), empty.count);
  }

  // Independent range tests on the two centers of the box compound and two spheres.
  const query overlap{query_kind::overlap, {0, 5, 0}, {}, cooked.probe.GetPtr()};
  const query shifted{query_kind::overlap, {4, 5, 0}, {}, cooked.probe.GetPtr()};
  const auto before = physics.run(overlap, scratch, 8, lookup_mode::index);
  std::array<uint64_t, 8> previous, current, entered, exited;
  const auto prior = std::span(previous).first(world::objects(before.hits(), previous));
  if (before.status != query_status::ok || before.count != 2 || prior.size() != 1 || prior[0] != compound.key) {
    utils::error{}("PHY02 overlap: expected two hits on compound {}, got status {}, {} hits and {} targets",
                   compound.key, int(before.status), before.count, prior.size());
  }
  const auto after = physics.run(shifted, scratch, 8, lookup_mode::index);
  const auto present = std::span(current).first(world::objects(after.hits(), current));
  const auto entering = std::set_difference(present.begin(), present.end(), prior.begin(), prior.end(), entered.begin());
  const auto exiting = std::set_difference(prior.begin(), prior.end(), present.begin(), present.end(), exited.begin());
  if (entering - entered.begin() != 2 || entered[0] != twin_a.key || entered[1] != twin_b.key ||
      exiting - exited.begin() != 1 || exited[0] != compound.key) {
    utils::error{}("PHY02 overlap: expected two spheres to enter and the compound to exit, got {} entering and {} exiting",
                   entering - entered.begin(), exiting - exited.begin());
  }

  const auto removed = physics.remove(twin_a);
  if (removed != world::command_status::ok) utils::error{}("PHY02 remove body {}: status {}", twin_a.key, int(removed));
  const auto replacement = add(physics, cooked.ball.GetPtr(), {4, 5, 0}, JPH::EMotionType::Static);
  if (uint32_t(replacement.key) != uint32_t(twin_a.key) || replacement.key == twin_a.key) {
    utils::error{}("PHY02 slot reuse: expected a new generation for body {}, got {}", twin_a.key, replacement.key);
  }
  const auto stale = physics.remove(twin_a);
  if (stale != world::command_status::stale) utils::error{}("PHY02 remove stale body {}: status {}", twin_a.key, int(stale));
  publish(physics);
  const auto updated = physics.run(ray, scratch, 8, lookup_mode::index);
  if (updated.count != 4 || updated.hits().back().key != replacement.key) {
    utils::error{}("PHY02 ray after slot reuse: expected 4 hits ending with body {}, got {} hits", replacement.key, updated.count);
  }
  advance(physics, 1); // A refused enqueue leaves the simulation able to continue.
}

void verify_contacts(const shapes& cooked) {
  thread::atomic_pool pool(1);
  limits cap;
  world physics(cap, pair_storage::hash, event_storage::lanes, 0, false, pool);
  add(physics, cooked.floor.GetPtr(), {0, -0.5f, 0}, JPH::EMotionType::Static);
  const auto body = add(physics, cooked.ball.GetPtr(), {0, 0.44f, 0}, JPH::EMotionType::Dynamic);
  publish(physics);
  bool slept = false;
  uint32_t tick = 0;
  for (; tick < 260 && !slept; ++tick) {
    advance(physics, tick + 1);
    slept = !physics.active(body);
  }
  if (!slept) utils::error{}("PHY02 sleep: body {} still active after {} ticks", body.key, tick);
  const query overlap{query_kind::overlap, {0, 0.45f, 0}, {}, cooked.probe.GetPtr()};
  std::array<hit, 8> scratch;
  const auto hits = physics.run(overlap, scratch, 8, lookup_mode::index).hits();
  if (std::ranges::none_of(hits, [body](const auto& h) { return h.key == body.key; })) {
    utils::error{}("PHY02 overlap: expected sleeping body {} among {} hits", body.key, hits.size());
  }
  physics.impulse(body, {0.005f, 0, 0});
  if (!physics.active(body)) utils::error{}("PHY02 impulse: body {} stayed asleep", body.key);
  bool active_contact = false;
  for (uint32_t i = 0; i < 8 && !active_contact; ++i) {
    advance(physics, ++tick);
    active_contact = std::any_of(physics.contacts().begin(), physics.contacts().end(),
      [body](const auto& event) { return event.pair.second == body.key && event.phase != contact_phase::removed; });
  }
  if (!active_contact) utils::error{}("PHY02 removal fixture: body {} had no contact in 8 ticks after waking", body.key);
  const auto removed = physics.remove(body);
  if (removed != world::command_status::ok) utils::error{}("PHY02 remove touching body {}: status {}", body.key, int(removed));
  const auto replacement = add(physics, cooked.box.GetPtr(), {4, 1, 0}, JPH::EMotionType::Dynamic);
  publish(physics);
  advance(physics, ++tick);
  const bool removal_reported = std::ranges::any_of(physics.contacts(), [body](const auto& event) {
    return event.pair.second == body.key && event.phase == contact_phase::removed;
  });
  if (replacement.key == body.key || !removal_reported) {
    utils::error{}("PHY02 contact removal: old body {}, replacement {}, removal published {}", body.key, replacement.key, removal_reported);
  }
}

void verify_pair_replacement(const shapes& cooked) {
  thread::atomic_pool pool(1);
  for (auto pairs : {pair_storage::linear, pair_storage::hash}) {
    limits cap;
    cap.pairs = 1;
    world physics(cap, pairs, event_storage::lanes, 0, false, pool);
    const auto left = add(physics, cooked.box.GetPtr(), {-2, -0.45f, 0}, JPH::EMotionType::Static);
    const auto right = add(physics, cooked.box.GetPtr(), {2, -0.45f, 0}, JPH::EMotionType::Static);
    const auto old_body = add(physics, cooked.ball.GetPtr(), {2, 0.44f, 0}, JPH::EMotionType::Dynamic);
    publish(physics);
    advance(physics, 1);

    const auto first = physics.contacts();
    if (first.size() != 1 || first[0].pair.first != right.key || first[0].pair.second != old_body.key ||
        first[0].phase != contact_phase::added) {
      utils::error{}("PHY02 pair replacement: expected one initial contact between {} and {}, got {} events",
                     right.key, old_body.key, first.size());
    }

    const auto removed = physics.remove(old_body);
    if (removed != world::command_status::ok) {
      utils::error{}("PHY02 pair replacement: removing body {} returned status {}", old_body.key, int(removed));
    }
    const auto new_body = add(physics, cooked.ball.GetPtr(), {-2, 0.44f, 0}, JPH::EMotionType::Dynamic);
    publish(physics);
    advance(physics, 2);

    const auto next = physics.contacts();
    if (next.size() != 2 || next[0].pair.first != left.key || next[0].pair.second != new_body.key ||
        next[0].phase != contact_phase::added || next[1].pair.first != right.key ||
        next[1].pair.second != old_body.key || next[1].phase != contact_phase::removed) {
      utils::error{}("PHY02 pair replacement: expected an added contact for {} and a removed contact for {}, got {} events",
                     new_body.key, old_body.key, next.size());
    }
  }
}

void verify_overflows(const shapes& cooked) {
  thread::atomic_pool pool(1);
  for (auto expected : {contact_status::event_capacity, contact_status::pair_capacity}) {
    limits cap;
    if (expected == contact_status::event_capacity) cap.events = 1;
    else cap.pairs = 1;
    world physics(cap, pair_storage::hash, event_storage::lanes, 0, false, pool);
    add(physics, cooked.floor.GetPtr(), {0, -0.5f, 0}, JPH::EMotionType::Static);
    for (float x : {-1.0f, 1.0f}) {
      add(physics, cooked.ball.GetPtr(), {x, 0.44f, 0}, JPH::EMotionType::Dynamic);
    }
    publish(physics);
    const auto step = physics.step();
    if (step.contacts != expected || !physics.contacts().empty()) {
      utils::error{}("PHY02 contact overflow: expected status {} and no publication, got status {} and {} events",
                     int(expected), int(step.contacts), physics.contacts().size());
    }
  }

  limits cap;
  cap.solver_pairs = 1;
  cap.solver_contacts = 1;
  world physics(cap, pair_storage::hash, event_storage::shared, 0, false, pool);
  for (int i = 0; i < 24; ++i) {
    add(physics, cooked.box.GetPtr(), {0.015f * i, 1, 0}, JPH::EMotionType::Dynamic);
  }
  publish(physics);
  const expect_solver_limit expected;
  const auto step = physics.step();
  if (step.solver == JPH::EPhysicsUpdateError::None || !physics.contacts().empty()) {
    utils::error{}("PHY02 solver overflow: expected failure flags and no publication, got flags {} and {} events",
                   uint32_t(step.solver), physics.contacts().size());
  }
}

void verify_pair_storage() {
  constexpr size_t capacity = 512;
  constexpr size_t iterations = 8192;
  for (size_t count : {size_t{32}, size_t{256}, capacity}) {
    for (auto mode : {pair_storage::linear, pair_storage::hash}) {
      active_pairs pairs(capacity, mode);
      const auto key = [](size_t i) { return pair_key{uint32_t(i + 1), 3, 0x01000001, 7}; };
      const auto value = [](size_t i) { return shape_pair{uint64_t(i + 1), 0x100000002ull, 3, 7}; };
      for (size_t i = 0; i < count; ++i) {
        if (!pairs.upsert(key(i), value(i))) utils::error{}("PHY02 pair table: insert {} failed before capacity {}", i, capacity);
      }
      const auto before = allocation_totals();
      const auto begin = std::chrono::steady_clock::now();
      for (size_t i = 0; i < iterations; ++i) {
        const auto index = (i * 73) % count;
        shape_pair old;
        if (!pairs.erase(key(index), old) || old != value(index)) {
          utils::error{}("PHY02 pair churn: erase did not return pair {} at iteration {}", index, i);
        }
        if (!pairs.upsert(key(index), value(index))) {
          utils::error{}("PHY02 pair churn: reinsertion of pair {} failed at iteration {}", index, i);
        }
      }
      const auto elapsed = micros(std::chrono::steady_clock::now() - begin);
      const auto allocations = allocation_delta(before).engine;
      if (allocations.calls != 0) {
        utils::error{}("PHY02 pair churn: {} allocations ({} bytes) with {} pairs", allocations.calls, allocations.bytes, count);
      }
      if (count == capacity && pairs.upsert(key(capacity), value(capacity))) {
        utils::error{}("PHY02 pair table: admitted another key at capacity {}", capacity);
      }
      for (size_t i = 0; i < count; ++i) {
        shape_pair old;
        if (!pairs.erase(key(i), old) || old != value(i)) {
          utils::error{}("PHY02 pair table: erase did not return pair {} after churn of {} entries", i, count);
        }
      }
      std::printf("pair churn %s occupancy=%zu/%zu buffer=%zu B erase+insert=%.3f us\n",
        mode == pair_storage::linear ? "linear" : "hash", count, capacity, pairs.bytes(), elapsed / iterations);
    }
  }
}

void verify_adapter() {
  constexpr uint32_t capacity = 64;
  thread::atomic_pool pool(3);
  shared_jobs jobs(pool, 2, capacity, 1);
  auto* barrier = jobs.CreateBarrier();
  std::atomic<uint32_t> done{0};
  std::atomic<uint32_t> background{0};
  // Hold the pumps until the bounded ready queue is exactly full. The
  // barrier also executes jobs, leaving queued references for finish().
  std::array<JPH::JobHandle, capacity> handles;
  for (auto& handle : handles) {
    handle = jobs.CreateJob("PHY02 saturation", JPH::Color::sWhite,
                            [&done] { done.fetch_add(1, std::memory_order_relaxed); }, 0);
    barrier->AddJob(handle);
  }
  if (jobs.peak() != capacity) {
    utils::error{}("PHY02 jobs saturation: ready queue peaked at {}, expected {}", jobs.peak(), capacity);
  }
  pool.submit([&background] { background.store(1, std::memory_order_relaxed); });
  jobs.start();
  jobs.WaitForJobs(barrier);
  jobs.DestroyBarrier(barrier);
  jobs.finish();
  if (done != capacity || background != 1) {
    utils::error{}("PHY02 jobs saturation: completed {}/{} Jolt jobs and {}/1 background jobs",
                   done.load(std::memory_order_relaxed), capacity, background.load(std::memory_order_relaxed));
  }

  for (auto& handle : handles) {
    handle = {};
  }
  barrier = jobs.CreateBarrier();
  const auto child = jobs.CreateJob("PHY02 child", JPH::Color::sWhite,
                                   [&done] { done.fetch_add(1, std::memory_order_relaxed); }, 1);
  const auto parent = jobs.CreateJob("PHY02 parent", JPH::Color::sWhite, [&child, barrier] {
    barrier->AddJob(child);
    child.RemoveDependency();
  }, 1);
  barrier->AddJob(parent);
  jobs.start();
  parent.RemoveDependency();
  jobs.WaitForJobs(barrier);
  jobs.DestroyBarrier(barrier);
  jobs.finish();
  if (done != capacity + 1) {
    utils::error{}("PHY02 live barrier: completed {} jobs, expected {} including the added child",
                   done.load(std::memory_order_relaxed), capacity + 1);
  }
}

void compare(const run_report& reference, const run_report& candidate) {
  if (reference.tick_ends.size() != candidate.tick_ends.size()) {
    utils::error{}("PHY02 transcript: {} ticks, expected {}", candidate.tick_ends.size(), reference.tick_ends.size());
  }
  size_t previous_ref = 0;
  size_t previous_candidate = 0;
  for (size_t tick = 0; tick < reference.tick_ends.size(); ++tick) {
    const auto expected = std::span(reference.trace).subspan(previous_ref, reference.tick_ends[tick] - previous_ref);
    const auto actual = std::span(candidate.trace).subspan(previous_candidate, candidate.tick_ends[tick] - previous_candidate);
    if (!std::ranges::equal(expected, actual)) utils::error{}("PHY02 state, contacts or queries differ at tick {}", tick + 1);
    previous_ref = reference.tick_ends[tick];
    previous_candidate = candidate.tick_ends[tick];
  }
}

void verify(const shapes& cooked, settings config) {
  verify_pair_storage();
  verify_queries(cooked);
  verify_contacts(cooked);
  verify_pair_replacement(cooked);
  verify_overflows(cooked);
  verify_adapter();
  for (bool dense : {false, true}) {
    config.dense = dense;
    config.workers = 0;
    config.shared_jobs = false;
    config.pairs = pair_storage::linear;
    config.events = event_storage::shared;
    config.lookup = lookup_mode::linear;
    const auto reference = run(cooked, config, true);
    config.pairs = pair_storage::hash;
    for (bool shared : {false, true}) {
      config.shared_jobs = shared;
      for (uint32_t workers : std::array<uint32_t, 3>{0, 2, 4}) {
        config.workers = workers;
        config.events = workers == 2 ? event_storage::shared : event_storage::lanes;
        config.lookup = workers == 2 ? lookup_mode::user_data : lookup_mode::index;
        compare(reference, run(cooked, config, true));
      }
    }
  }
  std::puts("PHY02 verified full per-tick state, contact and query transcripts");
}

void scratch_limit_probe(const shapes& cooked) {
  thread::atomic_pool pool(1);
  limits cap;
  cap.scratch = 1;
  world physics(cap, pair_storage::linear, event_storage::shared, 0, false, pool);
  add(physics, cooked.ball.GetPtr(), {0, 2, 0}, JPH::EMotionType::Dynamic);
  publish(physics);
  advance(physics, 1);
  utils::error{}("PHY02 scratch probe did not exhaust its arena");
}

} // namespace
} // namespace devils_engine::phy02

int main(int argc, char** argv) {
  using namespace phy02;
  settings config;
  bool verify_mode = false;
  bool scratch_mode = false;
  std::string scene = "dense", jobs = "stock", pairs = "hash", events = "lanes", lookup = "index";
  CLI::App cli("Headless Jolt world budgets, contact pairs and jobs");
  cli.add_flag("--verify", verify_mode, "Compare complete per-tick transcripts and run negative fixtures");
  cli.add_flag("--scratch-limit-probe", scratch_mode, "Child-process test for insufficient solver scratch");
  cli.add_option("--bodies", config.bodies, "Dynamic bodies")->check(CLI::Range(uint32_t{16}, uint32_t{512}));
  cli.add_option("--steps", config.steps, "Fixed ticks including warmup")->check(CLI::Range(warmup_ticks + uint32_t{1}, uint32_t{480}));
  cli.add_option("--workers", config.workers, "Jolt workers besides the caller")->check(CLI::Range(uint32_t{0}, uint32_t{4}));
  cli.add_option("--scene", scene, "Fixture")->check(CLI::IsMember({"dense", "sparse"}));
  cli.add_option("--jobs", jobs, "Executor")->check(CLI::IsMember({"stock", "shared"}));
  cli.add_option("--pairs", pairs, "Active pair lookup")->check(CLI::IsMember({"linear", "hash"}));
  cli.add_option("--events", events, "Contact callback buffer")->check(CLI::IsMember({"shared", "lanes"}));
  cli.add_option("--lookup", lookup, "Backend body ID lookup")->check(CLI::IsMember({"linear", "index", "user-data"}));
  if (const auto result = playground::parse_cli(cli, argc, argv)) return *result;

  config.dense = scene == "dense";
  config.shared_jobs = jobs == "shared";
  config.pairs = pairs == "linear" ? pair_storage::linear : pair_storage::hash;
  config.events = events == "shared" ? event_storage::shared : event_storage::lanes;
  config.lookup = lookup == "linear" ? lookup_mode::linear
                    : lookup == "user-data" ? lookup_mode::user_data : lookup_mode::index;
  try {
    const jolt_runtime runtime;
    const auto before = allocation_totals();
    const shapes cooked;
    report_allocations("shape cooking", allocation_delta(before));
    if (scratch_mode) scratch_limit_probe(cooked);
    else if (verify_mode) verify(cooked, config);
    else run(cooked, config, false);
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "PHY02: %s\n", failure.what());
    return 1;
  }
}
