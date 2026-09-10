#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Math/Trigonometry.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "devils_engine/utils/deterministic_math.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace layers {
constexpr JPH::ObjectLayer static_body = 0;
constexpr JPH::ObjectLayer moving_body = 1;
constexpr JPH::uint count = 2;
}

namespace broad_phase_layers {
constexpr JPH::BroadPhaseLayer static_body{0};
constexpr JPH::BroadPhaseLayer moving_body{1};
constexpr JPH::uint count = 2;
}

void trace(const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertion_failed(
    const char* expression,
    const char* message,
    const char* file,
    JPH::uint line) {
  std::fprintf(
      stderr,
      "%s:%u: Jolt assertion `%s`: %s\n",
      file,
      line,
      expression,
      message == nullptr ? "" : message);
  return true;
}
#endif

// Jolt's allocator hooks, Factory and type registry are process-wide. An engine
// integration needs one owner above all physics worlds, rather than repeating
// HelloWorld initialization in each scene.
class jolt_runtime final {
public:
  jolt_runtime() {
    JPH::RegisterDefaultAllocator();
    JPH::Trace = trace;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = assertion_failed;
#endif
    if (JPH::Factory::sInstance != nullptr) {
      std::fputs("PHY01: a Jolt factory already exists\n", stderr);
      std::abort();
    }
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
  }

  ~jolt_runtime() {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    JPH::Trace = nullptr;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = nullptr;
#endif
  }

  jolt_runtime(const jolt_runtime&) = delete;
  jolt_runtime& operator=(const jolt_runtime&) = delete;
};

struct body_handle {
  uint32_t value = 0;
  uint64_t world = 0;

  explicit operator bool() const noexcept { return value != 0 && world != 0; }
  friend bool operator==(body_handle, body_handle) = default;
};

struct shape_handle {
  uint32_t value = 0;
  uint64_t catalog = 0;

  explicit operator bool() const noexcept { return value != 0 && catalog != 0; }
  friend bool operator==(shape_handle, shape_handle) = default;
};

// Cooking is separate from world publication. Immutable Jolt shapes are
// reference counted and can be shared by query and simulation worlds.
class shape_catalog final {
public:
  shape_handle cook_box(JPH::Vec3Arg half_extent) {
    JPH::BoxShapeSettings settings(half_extent);
    return store(settings.Create());
  }

  shape_handle cook_sphere(float radius) {
    JPH::SphereShapeSettings settings(radius);
    return store(settings.Create());
  }

  JPH::ShapeRefC resolve(shape_handle handle) const {
    if (!handle || handle.catalog != token_ || handle.value > shapes_.size()) {
      std::fputs("PHY01: stale or foreign shape handle\n", stderr);
      std::abort();
    }
    return shapes_[handle.value - 1];
  }

private:
  shape_handle store(const JPH::ShapeSettings::ShapeResult& cooked) {
    if (cooked.HasError()) {
      std::fprintf(stderr, "PHY01: shape cooking failed: %s\n", cooked.GetError().c_str());
      return {};
    }
    shapes_.push_back(cooked.Get());
    return {static_cast<uint32_t>(shapes_.size()), token_};
  }

  inline static std::atomic<uint64_t> next_token_{1};
  const uint64_t token_ = next_token_.fetch_add(1, std::memory_order_relaxed);
  std::vector<JPH::ShapeRefC> shapes_;
};

class contact_counter final : public JPH::ContactListener {
public:
  void OnContactAdded(
      const JPH::Body&,
      const JPH::Body&,
      const JPH::ContactManifold&,
      JPH::ContactSettings&) override {
    added_.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t added() const noexcept {
    return added_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> added_{0};
};

struct ray_hit {
  body_handle body;
  float fraction = 1.0f;
};

// This is deliberately a playground-local seam. It hides Jolt body IDs from
// callers, owns every object whose address PhysicsSystem retains and makes the
// fixed-step/job-system boundary explicit. It is evidence for a future
// libs/physics API, not that API guessed in advance.
class physics_world final {
public:
  enum class mode { queries_only, simulation };

  physics_world(
      const shape_catalog& shapes,
      const uint32_t max_bodies,
      const mode world_mode,
      const uint32_t worker_threads)
      : shapes_(shapes),
        pair_filter_(layers::count),
        broad_phase_(layers::count, broad_phase_layers::count) {
    pair_filter_.EnableCollision(layers::static_body, layers::moving_body);
    pair_filter_.EnableCollision(layers::moving_body, layers::moving_body);
    broad_phase_.MapObjectToBroadPhaseLayer(
        layers::static_body, broad_phase_layers::static_body);
    broad_phase_.MapObjectToBroadPhaseLayer(
        layers::moving_body, broad_phase_layers::moving_body);
    // This table snapshots both source tables in its constructor. Constructing
    // it before the mappings above silently disables all broadphase pairs.
    object_vs_broad_phase_ = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
        broad_phase_, broad_phase_layers::count, pair_filter_, layers::count);

    if (world_mode == mode::simulation) {
      temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(8 * 1024 * 1024);
      job_system_ = std::make_unique<JPH::JobSystemThreadPool>();
      job_system_->SetThreadInitFunction([this](int) {
        worker_starts_.fetch_add(1, std::memory_order_relaxed);
      });
      job_system_->Init(
          JPH::cMaxPhysicsJobs,
          JPH::cMaxPhysicsBarriers,
          static_cast<int>(worker_threads));
    }

    const JPH::uint max_pairs = std::max<JPH::uint>(1024, max_bodies * 8);
    const JPH::uint max_contacts = std::max<JPH::uint>(1024, max_bodies * 4);
    system_.Init(
        max_bodies,
        0,
        max_pairs,
        max_contacts,
        broad_phase_,
        *object_vs_broad_phase_,
        pair_filter_);
    system_.SetContactListener(&contacts_);
  }

  ~physics_world() {
    auto& bodies = system_.GetBodyInterface();
    for (auto it = body_slots_.rbegin(); it != body_slots_.rend(); ++it) {
      if (it->IsInvalid()) continue;
      bodies.RemoveBody(*it);
      bodies.DestroyBody(*it);
    }
  }

  physics_world(const physics_world&) = delete;
  physics_world& operator=(const physics_world&) = delete;

  body_handle queue_body(
      shape_handle shape,
      JPH::RVec3Arg position,
      JPH::EMotionType motion) {
    const body_handle handle{
        static_cast<uint32_t>(body_slots_.size() + 1), world_token_};
    body_slots_.emplace_back();
    pending_.push_back({
        handle,
        shapes_.resolve(shape),
        position,
        motion,
        motion == JPH::EMotionType::Static ? layers::static_body : layers::moving_body,
        JPH::Vec3::sZero()});
    return handle;
  }

  void add_impulse(body_handle handle, JPH::Vec3Arg impulse) {
    validate(handle);
    if (body_slots_[handle.value - 1].IsInvalid()) {
      for (auto& body : pending_) {
        if (body.handle == handle) {
          body.impulse += impulse;
          return;
        }
      }
      std::fputs("PHY01: body handle has no pending command\n", stderr);
      std::abort();
    }
    system_.GetBodyInterface().AddImpulse(resolve(handle), impulse);
  }

  size_t publish_pending() {
    if (pending_.empty()) return 0;
    auto& bodies = system_.GetBodyInterface();
    std::vector<JPH::BodyID> batch;
    batch.reserve(pending_.size());
    for (const pending_body& pending : pending_) {
      const JPH::BodyCreationSettings settings(
          pending.shape,
          pending.position,
          JPH::Quat::sIdentity(),
          pending.motion,
          pending.layer);
      JPH::Body* body = bodies.CreateBody(settings);
      if (body == nullptr) {
        for (const JPH::BodyID created : batch) bodies.DestroyBody(created);
        std::fputs("PHY01: body capacity exhausted during batch prepare\n", stderr);
        std::abort();
      }
      const JPH::BodyID id = body->GetID();
      body_slots_[pending.handle.value - 1] = id;
      batch.push_back(id);
    }

    // AddBodiesPrepare may reorder this scratch list, so stable slot->BodyID
    // publication lives in body_slots_ and is never inferred from batch order.
    const auto add_state = bodies.AddBodiesPrepare(batch.data(), int(batch.size()));
    bodies.AddBodiesFinalize(
        batch.data(), int(batch.size()), add_state, JPH::EActivation::Activate);
    for (const pending_body& pending : pending_) {
      if (!pending.impulse.IsNearZero())
        bodies.AddImpulse(resolve(pending.handle), pending.impulse);
    }
    const size_t count = pending_.size();
    pending_.clear();
    return count;
  }

  void optimize() { system_.OptimizeBroadPhase(); }

  void step(float delta_time) {
    if (temp_allocator_ == nullptr || job_system_ == nullptr) {
      std::fputs("PHY01: a queries-only world cannot be stepped\n", stderr);
      std::abort();
    }
    constexpr int collision_steps = 1;
    const JPH::EPhysicsUpdateError error =
        system_.Update(delta_time, collision_steps, temp_allocator_.get(), job_system_.get());
    if (error != JPH::EPhysicsUpdateError::None) {
      std::fprintf(stderr, "PHY01: PhysicsSystem::Update error 0x%x\n", unsigned(error));
      std::abort();
    }
  }

  ray_hit cast_ray(JPH::RVec3Arg origin, JPH::Vec3Arg direction) const {
    JPH::RayCastResult result;
    if (!system_.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(origin, direction), result)) {
      return {};
    }

    for (size_t i = 0; i < body_slots_.size(); ++i) {
      if (body_slots_[i] == result.mBodyID) {
        return {body_handle{static_cast<uint32_t>(i + 1), world_token_}, result.mFraction};
      }
    }
    std::fputs("PHY01: ray returned an unowned body\n", stderr);
    std::abort();
  }

  size_t overlap_point(JPH::RVec3Arg point) const {
    JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
    system_.GetNarrowPhaseQuery().CollidePoint(point, collector);
    return collector.mHits.size();
  }

  std::vector<std::byte> snapshot() const {
    std::vector<std::byte> bytes;
    if (!pending_.empty()) {
      std::fputs("PHY01: cannot snapshot an unpublished body batch\n", stderr);
      std::abort();
    }
    bytes.reserve(body_slots_.size() * 64);
    const auto& bodies = system_.GetBodyInterface();
    for (const JPH::BodyID id : body_slots_) {
      const JPH::RVec3 position = bodies.GetPosition(id);
      const JPH::Quat rotation = bodies.GetRotation(id);
      const JPH::Vec3 linear = bodies.GetLinearVelocity(id);
      const JPH::Vec3 angular = bodies.GetAngularVelocity(id);
      append(bytes, id.GetIndexAndSequenceNumber());
      append(bytes, position.GetX());
      append(bytes, position.GetY());
      append(bytes, position.GetZ());
      append(bytes, rotation.GetX());
      append(bytes, rotation.GetY());
      append(bytes, rotation.GetZ());
      append(bytes, rotation.GetW());
      append(bytes, linear.GetX());
      append(bytes, linear.GetY());
      append(bytes, linear.GetZ());
      append(bytes, angular.GetX());
      append(bytes, angular.GetY());
      append(bytes, angular.GetZ());
      append(bytes, uint8_t(bodies.IsActive(id)));
    }
    return bytes;
  }

  uint32_t worker_starts() const noexcept {
    return worker_starts_.load(std::memory_order_relaxed);
  }

  uint64_t contacts_added() const noexcept { return contacts_.added(); }

private:
  template <typename T>
  static void append(std::vector<std::byte>& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto old_size = bytes.size();
    bytes.resize(old_size + sizeof(T));
    std::memcpy(bytes.data() + old_size, &value, sizeof(T));
  }

  struct pending_body {
    body_handle handle;
    JPH::ShapeRefC shape;
    JPH::RVec3 position;
    JPH::EMotionType motion;
    JPH::ObjectLayer layer;
    JPH::Vec3 impulse;
  };

  void validate(body_handle handle) const {
    if (!handle || handle.world != world_token_ || handle.value > body_slots_.size()) {
      std::fputs("PHY01: stale or foreign body handle\n", stderr);
      std::abort();
    }
  }

  JPH::BodyID resolve(body_handle handle) const {
    validate(handle);
    const JPH::BodyID id = body_slots_[handle.value - 1];
    if (id.IsInvalid()) {
      std::fputs("PHY01: body batch has not been published\n", stderr);
      std::abort();
    }
    return id;
  }

  const shape_catalog& shapes_;
  JPH::ObjectLayerPairFilterTable pair_filter_;
  JPH::BroadPhaseLayerInterfaceTable broad_phase_;
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase_;
  contact_counter contacts_;
  JPH::PhysicsSystem system_;
  std::atomic<uint32_t> worker_starts_{0};
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
  std::vector<JPH::BodyID> body_slots_;
  std::vector<pending_body> pending_;
  inline static std::atomic<uint64_t> next_world_token_{1};
  const uint64_t world_token_ =
      next_world_token_.fetch_add(1, std::memory_order_relaxed);
};

struct options {
  uint32_t steps = 240;
  uint32_t bodies = 48;
  uint32_t threads = 0;
  bool threads_were_set = false;
  bool verify = false;
};

bool parse_u32(std::string_view text, uint32_t& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

options parse_options(int argc, const char** argv) {
  options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    auto parse_named = [&](std::string_view prefix, uint32_t& target) {
      return argument.starts_with(prefix) && parse_u32(argument.substr(prefix.size()), target);
    };

    if (argument == "--verify") {
      result.verify = true;
    } else if (argument.starts_with("--steps=")) {
      if (!parse_named("--steps=", result.steps) || result.steps == 0) std::abort();
    } else if (argument.starts_with("--bodies=")) {
      if (!parse_named("--bodies=", result.bodies) || result.bodies == 0) std::abort();
    } else if (argument.starts_with("--threads=")) {
      if (!parse_named("--threads=", result.threads) || result.threads > 64) std::abort();
      result.threads_were_set = true;
    } else if (argument == "--help" || argument == "-h") {
      std::cout
          << "PHY01 Jolt basics\n"
          << "  --steps=N    fixed 60 Hz steps (default 240)\n"
          << "  --bodies=N   dynamic bodies in the fixture (default 48)\n"
          << "  --threads=N  Jolt worker threads; main thread also works\n"
          << "  --verify     compare 0-worker and multi-worker snapshots bitwise\n";
      std::exit(0);
    } else {
      std::fprintf(stderr, "PHY01: unknown argument: %.*s\n", int(argument.size()), argument.data());
      std::abort();
    }
  }

  if (!result.threads_were_set) {
    const uint32_t hardware = std::thread::hardware_concurrency();
    result.threads = std::clamp(hardware > 1 ? hardware - 1 : 1u, 1u, 4u);
  }
  return result;
}

uint64_t hash_bytes(std::span<const std::byte> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (const std::byte value : bytes) {
    hash ^= std::to_integer<uint8_t>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

struct run_result {
  std::vector<std::byte> state;
  ray_hit ray;
  uint32_t workers_started = 0;
  uint64_t contacts_added = 0;
  size_t published = 0;
};

struct fixture_shapes {
  shape_catalog catalog;
  shape_handle floor;
  shape_handle box;
  shape_handle sphere;
  shape_handle query_box;

  fixture_shapes() {
    floor = catalog.cook_box(JPH::Vec3(16.0f, 1.0f, 16.0f));
    box = catalog.cook_box(JPH::Vec3::sReplicate(0.48f));
    sphere = catalog.cook_sphere(0.48f);
    query_box = catalog.cook_box(JPH::Vec3::sReplicate(1.0f));
    if (!floor || !box || !sphere || !query_box) std::abort();
  }
};

run_result run_scene(
    const options& opts,
    const fixture_shapes& shapes,
    uint32_t worker_threads) {
  physics_world world(
      shapes.catalog,
      opts.bodies + 8,
      physics_world::mode::simulation,
      worker_threads);
  world.queue_body(shapes.floor, JPH::RVec3(0.0f, -1.0f, 0.0f), JPH::EMotionType::Static);

  for (uint32_t i = 0; i < opts.bodies; ++i) {
    const uint32_t x = i % 8;
    const uint32_t layer = i / 8;
    const JPH::RVec3 position(
        (float(x) - 3.5f) * 1.05f,
        0.6f + float(layer) * 1.1f,
        float(int(i % 3) - 1) * 0.08f);
    const body_handle body = (i & 1u) == 0
        ? world.queue_body(shapes.box, position, JPH::EMotionType::Dynamic)
        : world.queue_body(shapes.sphere, position, JPH::EMotionType::Dynamic);

    const float angle = float(i) * 0.37f;
    world.add_impulse(
        body,
        JPH::Vec3(
            0.06f * devils_engine::utils::deterministic::cos(angle),
            0.0f,
            0.06f * devils_engine::utils::deterministic::sin(angle)));
  }

  const size_t published = world.publish_pending();
  world.optimize();
  constexpr float fixed_delta = 1.0f / 60.0f;
  for (uint32_t step = 0; step < opts.steps; ++step) world.step(fixed_delta);

  run_result result;
  result.ray = world.cast_ray(JPH::RVec3(0.0f, 20.0f, 0.0f), JPH::Vec3(0.0f, -40.0f, 0.0f));
  result.state = world.snapshot();
  result.workers_started = world.worker_starts();
  result.contacts_added = world.contacts_added();
  result.published = published;
  return result;
}

struct query_probe {
  bool invisible_before_commit = false;
  bool visible_after_commit = false;
  size_t overlap_count = 0;
};

query_probe probe_query_only(const fixture_shapes& shapes) {
  physics_world world(shapes.catalog, 8, physics_world::mode::queries_only, 0);
  world.queue_body(
      shapes.query_box, JPH::RVec3(0.0f, 0.0f, 0.0f), JPH::EMotionType::Static);
  const ray_hit before =
      world.cast_ray(JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f));
  const size_t overlap_before = world.overlap_point(JPH::RVec3::sZero());
  const size_t published = world.publish_pending();
  const ray_hit after =
      world.cast_ray(JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f));
  const size_t overlap_after = world.overlap_point(JPH::RVec3::sZero());
  return {
      !before.body && overlap_before == 0,
      published == 1 && bool(after.body),
      overlap_after};
}

struct math_probe {
  uint64_t signature = 0;
  float max_unit_circle_error = 0.0f;
  bool matches_jolt = true;
};

math_probe probe_math() {
  constexpr std::array angles{
      -100.0f, -JPH::JPH_PI, -1.0f, -0.0f, 0.0f, 0.25f, 1.0f,
      JPH::JPH_PI, 10.0f, 100.0f};
  std::array<uint32_t, angles.size() * 2> bits{};
  math_probe result;
  for (size_t i = 0; i < angles.size(); ++i) {
    const auto value = devils_engine::utils::deterministic::sin_cos(angles[i]);
    const float sine = value.sine;
    const float cosine = value.cosine;
    bits[i * 2] = std::bit_cast<uint32_t>(sine);
    bits[i * 2 + 1] = std::bit_cast<uint32_t>(cosine);
    result.matches_jolt = result.matches_jolt
        && bits[i * 2] == std::bit_cast<uint32_t>(JPH::Sin(angles[i]))
        && bits[i * 2 + 1] == std::bit_cast<uint32_t>(JPH::Cos(angles[i]));
    result.max_unit_circle_error = std::max(
        result.max_unit_circle_error, std::abs(sine * sine + cosine * cosine - 1.0f));
  }
  result.signature = hash_bytes(std::as_bytes(std::span(bits)));
  return result;
}

} // namespace

int main(int argc, const char** argv) {
  const options opts = parse_options(argc, argv);
  jolt_runtime runtime;
  const fixture_shapes shapes;

  const math_probe math = probe_math();
  const query_probe queries = probe_query_only(shapes);
  const run_result threaded = run_scene(opts, shapes, opts.threads);

  std::cout << "PHY01 Jolt 5.6.0 basics\n"
            << "  build.cross_platform_deterministic="
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
            << "on\n"
#else
            << "off\n"
#endif
            << "  scene.steps=" << opts.steps
            << " bodies=" << opts.bodies
            << " workers=" << opts.threads
            << " workers_started=" << threaded.workers_started << '\n'
            << "  scene.state_hash=0x" << std::hex << hash_bytes(threaded.state) << std::dec
            << " contacts_added=" << threaded.contacts_added
            << " batch_published=" << threaded.published << '\n'
            << "  ray.hit_handle=" << threaded.ray.body.value
            << " fraction=" << threaded.ray.fraction << '\n'
            << "  query_only.before_commit="
            << (queries.invisible_before_commit ? "invisible" : "VISIBLE")
            << " after_commit=" << (queries.visible_after_commit ? "visible" : "MISSING")
            << " point_overlaps=" << queries.overlap_count << '\n'
            << "  math.sin_cos_hash=0x" << std::hex << math.signature << std::dec
            << " max_unit_circle_error=" << math.max_unit_circle_error
            << " matches_jolt=" << (math.matches_jolt ? "yes" : "NO") << '\n';

  if (!threaded.ray.body || threaded.contacts_added == 0
      || threaded.published != size_t(opts.bodies) + 1
      || threaded.workers_started != opts.threads || !math.matches_jolt
      || !queries.invisible_before_commit || !queries.visible_after_commit
      || queries.overlap_count != 1) {
    std::fputs("PHY01: basic scene or worker startup check failed\n", stderr);
    return 1;
  }

  if (opts.verify) {
    const run_result serial = run_scene(opts, shapes, 0);
    const bool state_equal = serial.state == threaded.state;
    // Handles from different worlds intentionally do not compare equal; the
    // stable slots and hit fractions should still correspond.
    const bool ray_equal = serial.ray.body.value == threaded.ray.body.value
        && std::bit_cast<uint32_t>(serial.ray.fraction)
            == std::bit_cast<uint32_t>(threaded.ray.fraction);
    std::cout << "  verify.serial_vs_threaded_state=" << (state_equal ? "bit-exact" : "DIFF")
              << " ray=" << (ray_equal ? "bit-exact" : "DIFF") << '\n';
    if (!state_equal || !ray_equal) return 1;
  }

  return 0;
}
