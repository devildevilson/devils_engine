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
#include "devils_engine/utils/deterministic_sort.h"

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
#include <mutex>
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
  uint32_t generation = 0;
  uint64_t world = 0;

  explicit operator bool() const noexcept {
    return value != 0 && generation != 0 && world != 0;
  }
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

enum class contact_phase : uint8_t { added, persisted, removed };

struct contact_event {
  contact_phase phase;
  body_handle first;
  body_handle second;
  uint32_t first_sub_shape;
  uint32_t second_sub_shape;
};

// Callbacks can arrive concurrently and their arrival order is not gameplay
// evidence. Bodies carry the engine handle in user data, and removed contacts
// are resolved through the pair cache populated while the bodies were readable.
class contact_collector final : public JPH::ContactListener {
public:
  explicit contact_collector(size_t max_contacts)
      : contact_capacity_(max_contacts), event_capacity_(max_contacts * 2) {
    active_.reserve(max_contacts);
    pending_.reserve(event_capacity_);
  }

  void OnContactAdded(
      const JPH::Body& first,
      const JPH::Body& second,
      const JPH::ContactManifold& manifold,
      JPH::ContactSettings&) override {
    record(contact_phase::added, first, second, manifold);
  }

  void OnContactPersisted(
      const JPH::Body& first,
      const JPH::Body& second,
      const JPH::ContactManifold& manifold,
      JPH::ContactSettings&) override {
    record(contact_phase::persisted, first, second, manifold);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
    std::scoped_lock lock(mutex_);
    const contact_key key{
        pair.GetBody1ID().GetIndexAndSequenceNumber(),
        pair.GetBody2ID().GetIndexAndSequenceNumber(),
        pair.GetSubShapeID1().GetValue(),
        pair.GetSubShapeID2().GetValue()};
    const auto it = std::find_if(active_.begin(), active_.end(), [&](const cached_contact& value) {
      return value.key == key;
    });
    if (it == active_.end()) {
      ++unresolved_removed_;
      return;
    }
    if (pending_.size() >= event_capacity_) {
      ++overflow_;
    } else {
      pending_.push_back({contact_phase::removed, it->first, it->second,
                          it->first_sub_shape, it->second_sub_shape});
    }
    active_.erase(it);
  }

  std::vector<contact_event> drain(uint64_t world_token) {
    std::vector<raw_event> raw;
    {
      std::scoped_lock lock(mutex_);
      // Allocation/copy happens on the caller after Update. The callback-owned
      // vector retains its prepared capacity for the next worker-thread batch.
      raw = pending_;
      pending_.clear();
    }
    std::vector<contact_event> events;
    events.reserve(raw.size());
    for (const raw_event& event : raw) {
      events.push_back({
          event.phase,
          {event.first.value, event.first.generation, world_token},
          {event.second.value, event.second.generation, world_token},
          event.first_sub_shape,
          event.second_sub_shape});
    }
    devils_engine::utils::deterministic_sort(
        events.begin(), events.end(), [](const contact_event& left, const contact_event& right) {
          const auto key = [](const contact_event& value) {
            return std::array<uint32_t, 7>{
                value.first.value, value.first.generation,
                value.second.value, value.second.generation,
                value.first_sub_shape, value.second_sub_shape,
                uint32_t(value.phase)};
          };
          return key(left) < key(right);
        });
    return events;
  }

  uint64_t unresolved_removed() const noexcept {
    return unresolved_removed_.load(std::memory_order_relaxed);
  }

  uint64_t overflow() const noexcept {
    return overflow_.load(std::memory_order_relaxed);
  }

private:
  struct local_handle {
    uint32_t value;
    uint32_t generation;
    friend bool operator==(local_handle, local_handle) = default;
  };

  struct contact_key {
    uint32_t first_body;
    uint32_t second_body;
    uint32_t first_sub_shape;
    uint32_t second_sub_shape;
    friend bool operator==(contact_key, contact_key) = default;
  };

  struct raw_event {
    contact_phase phase;
    local_handle first;
    local_handle second;
    uint32_t first_sub_shape;
    uint32_t second_sub_shape;
  };

  struct cached_contact {
    contact_key key;
    local_handle first;
    local_handle second;
    uint32_t first_sub_shape;
    uint32_t second_sub_shape;
  };

  static local_handle unpack(uint64_t value) noexcept {
    return {uint32_t(value), uint32_t(value >> 32)};
  }

  static bool less(local_handle left, local_handle right) noexcept {
    return left.value < right.value
        || (left.value == right.value && left.generation < right.generation);
  }

  void record(
      contact_phase phase,
      const JPH::Body& body1,
      const JPH::Body& body2,
      const JPH::ContactManifold& manifold) {
    const contact_key key{
        body1.GetID().GetIndexAndSequenceNumber(),
        body2.GetID().GetIndexAndSequenceNumber(),
        manifold.mSubShapeID1.GetValue(),
        manifold.mSubShapeID2.GetValue()};
    local_handle first = unpack(body1.GetUserData());
    local_handle second = unpack(body2.GetUserData());
    uint32_t first_sub_shape = manifold.mSubShapeID1.GetValue();
    uint32_t second_sub_shape = manifold.mSubShapeID2.GetValue();
    if (less(second, first)) {
      std::swap(first, second);
      std::swap(first_sub_shape, second_sub_shape);
    }

    std::scoped_lock lock(mutex_);
    const auto it = std::find_if(active_.begin(), active_.end(), [&](const cached_contact& value) {
      return value.key == key;
    });
    const cached_contact cached{key, first, second, first_sub_shape, second_sub_shape};
    if (it == active_.end()) {
      if (active_.size() >= contact_capacity_) {
        ++overflow_;
        return;
      }
      active_.push_back(cached);
    } else {
      *it = cached;
    }
    if (pending_.size() >= event_capacity_) {
      ++overflow_;
    } else {
      pending_.push_back({phase, first, second, first_sub_shape, second_sub_shape});
    }
  }

  const size_t contact_capacity_;
  const size_t event_capacity_;
  mutable std::mutex mutex_;
  std::vector<raw_event> pending_;
  std::vector<cached_contact> active_;
  std::atomic<uint64_t> unresolved_removed_{0};
  std::atomic<uint64_t> overflow_{0};
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
  enum class publish_status { published, nothing_pending, body_capacity_exhausted };

  struct publish_result {
    publish_status status;
    size_t count;
  };

  physics_world(
      const shape_catalog& shapes,
      const uint32_t max_bodies,
      const mode world_mode,
      const uint32_t worker_threads)
      : shapes_(shapes),
        pair_filter_(layers::count),
        broad_phase_(layers::count, broad_phase_layers::count),
        contacts_(std::max<JPH::uint>(1024, max_bodies * 4)) {
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
      if (it->state != slot_state::published) continue;
      bodies.RemoveBody(it->id);
      bodies.DestroyBody(it->id);
    }
  }

  physics_world(const physics_world&) = delete;
  physics_world& operator=(const physics_world&) = delete;

  body_handle queue_body(
      shape_handle shape,
      JPH::RVec3Arg position,
      JPH::EMotionType motion) {
    uint32_t slot_index = 0;
    if (free_slots_.empty()) {
      slot_index = static_cast<uint32_t>(body_slots_.size());
      body_slots_.push_back({});
    } else {
      slot_index = free_slots_.back();
      free_slots_.pop_back();
    }
    body_slot& slot = body_slots_[slot_index];
    slot.state = slot_state::pending;
    const body_handle handle{slot_index + 1, slot.generation, world_token_};
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
    if (!is_alive(handle)) {
      std::fputs("PHY01: stale or foreign body handle\n", stderr);
      std::abort();
    }
    if (body_slots_[handle.value - 1].state == slot_state::pending) {
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

  bool is_alive(body_handle handle) const noexcept {
    if (!handle || handle.world != world_token_ || handle.value > body_slots_.size()) return false;
    const body_slot& slot = body_slots_[handle.value - 1];
    return slot.generation == handle.generation && slot.state != slot_state::free;
  }

  bool destroy_body(body_handle handle) {
    if (!is_alive(handle)) return false;
    body_slot& slot = body_slots_[handle.value - 1];
    if (slot.state == slot_state::pending) {
      const auto it = std::find_if(pending_.begin(), pending_.end(), [&](const pending_body& value) {
        return value.handle == handle;
      });
      if (it == pending_.end()) std::abort();
      pending_.erase(it);
    } else {
      auto& bodies = system_.GetBodyInterface();
      bodies.RemoveBody(slot.id);
      bodies.DestroyBody(slot.id);
    }
    release_slot(handle.value - 1);
    return true;
  }

  publish_result publish_pending() {
    if (pending_.empty()) return {publish_status::nothing_pending, 0};
    auto& bodies = system_.GetBodyInterface();
    std::vector<JPH::BodyID> batch;
    batch.reserve(pending_.size());
    for (const pending_body& pending : pending_) {
      JPH::BodyCreationSettings settings(
          pending.shape,
          pending.position,
          JPH::Quat::sIdentity(),
          pending.motion,
          pending.layer);
      settings.mUserData = pack(pending.handle);
      JPH::Body* body = bodies.CreateBody(settings);
      if (body == nullptr) {
        for (const JPH::BodyID created : batch) bodies.DestroyBody(created);
        return {publish_status::body_capacity_exhausted, 0};
      }
      const JPH::BodyID id = body->GetID();
      batch.push_back(id);
    }

    // AddBodiesPrepare may reorder this scratch list, so stable slot->BodyID
    // publication lives in body_slots_ and is never inferred from batch order.
    const auto add_state = bodies.AddBodiesPrepare(batch.data(), int(batch.size()));
    bodies.AddBodiesFinalize(
        batch.data(), int(batch.size()), add_state, JPH::EActivation::Activate);
    for (size_t i = 0; i < pending_.size(); ++i) {
      const pending_body& pending = pending_[i];
      body_slot& slot = body_slots_[pending.handle.value - 1];
      // batch may have been shuffled, so recover the ID through the user data
      // attached to each created body rather than pairing by array index.
      const auto it = std::find_if(batch.begin(), batch.end(), [&](JPH::BodyID id) {
        return bodies.GetUserData(id) == pack(pending.handle);
      });
      if (it == batch.end()) std::abort();
      slot.id = *it;
      slot.state = slot_state::published;
      if (!pending.impulse.IsNearZero())
        bodies.AddImpulse(resolve(pending.handle), pending.impulse);
    }
    const size_t count = pending_.size();
    pending_.clear();
    return {publish_status::published, count};
  }

  void optimize() { system_.OptimizeBroadPhase(); }

  std::vector<contact_event> step(float delta_time) {
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
    return contacts_.drain(world_token_);
  }

  ray_hit cast_ray(JPH::RVec3Arg origin, JPH::Vec3Arg direction) const {
    JPH::RayCastResult result;
    if (!system_.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(origin, direction), result)) {
      return {};
    }

    for (size_t i = 0; i < body_slots_.size(); ++i) {
      const body_slot& slot = body_slots_[i];
      if (slot.state == slot_state::published && slot.id == result.mBodyID) {
        return {body_handle{static_cast<uint32_t>(i + 1), slot.generation, world_token_},
                result.mFraction};
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
    bytes.reserve(body_slots_.size() * 68);
    const auto& bodies = system_.GetBodyInterface();
    for (size_t i = 0; i < body_slots_.size(); ++i) {
      const body_slot& slot = body_slots_[i];
      if (slot.state != slot_state::published) continue;
      const JPH::BodyID id = slot.id;
      const JPH::RVec3 position = bodies.GetPosition(id);
      const JPH::Quat rotation = bodies.GetRotation(id);
      const JPH::Vec3 linear = bodies.GetLinearVelocity(id);
      const JPH::Vec3 angular = bodies.GetAngularVelocity(id);
      append(bytes, static_cast<uint32_t>(i + 1));
      append(bytes, slot.generation);
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

  uint64_t unresolved_removed_contacts() const noexcept {
    return contacts_.unresolved_removed();
  }

  uint64_t contact_overflow() const noexcept { return contacts_.overflow(); }

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

  enum class slot_state : uint8_t { free, pending, published };

  struct body_slot {
    JPH::BodyID id;
    uint32_t generation = 1;
    slot_state state = slot_state::free;
  };

  static uint64_t pack(body_handle handle) noexcept {
    return uint64_t(handle.generation) << 32 | handle.value;
  }

  void release_slot(uint32_t slot_index) {
    body_slot& slot = body_slots_[slot_index];
    slot.id = JPH::BodyID{};
    slot.state = slot_state::free;
    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    free_slots_.push_back(slot_index);
  }

  JPH::BodyID resolve(body_handle handle) const {
    if (!is_alive(handle)) {
      std::fputs("PHY01: stale or foreign body handle\n", stderr);
      std::abort();
    }
    const body_slot& slot = body_slots_[handle.value - 1];
    if (slot.state != slot_state::published) {
      std::fputs("PHY01: body batch has not been published\n", stderr);
      std::abort();
    }
    return slot.id;
  }

  const shape_catalog& shapes_;
  JPH::ObjectLayerPairFilterTable pair_filter_;
  JPH::BroadPhaseLayerInterfaceTable broad_phase_;
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_vs_broad_phase_;
  contact_collector contacts_;
  JPH::PhysicsSystem system_;
  std::atomic<uint32_t> worker_starts_{0};
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
  std::vector<body_slot> body_slots_;
  std::vector<uint32_t> free_slots_;
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

template <typename T>
void append_value(std::vector<std::byte>& bytes, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const size_t old_size = bytes.size();
  bytes.resize(old_size + sizeof(T));
  std::memcpy(bytes.data() + old_size, &value, sizeof(T));
}

void append_contact_step(
    std::vector<std::byte>& bytes,
    std::span<const contact_event> events,
    uint64_t& added_count) {
  append_value(bytes, static_cast<uint32_t>(events.size()));
  for (const contact_event& event : events) {
    append_value(bytes, uint8_t(event.phase));
    append_value(bytes, event.first.value);
    append_value(bytes, event.first.generation);
    append_value(bytes, event.second.value);
    append_value(bytes, event.second.generation);
    append_value(bytes, event.first_sub_shape);
    append_value(bytes, event.second_sub_shape);
    if (event.phase == contact_phase::added) ++added_count;
  }
}

struct run_result {
  std::vector<std::byte> state;
  std::vector<std::byte> contact_stream;
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

  const auto publication = world.publish_pending();
  if (publication.status != physics_world::publish_status::published) std::abort();
  world.optimize();
  constexpr float fixed_delta = 1.0f / 60.0f;

  run_result result;
  for (uint32_t step = 0; step < opts.steps; ++step) {
    const std::vector<contact_event> events = world.step(fixed_delta);
    append_contact_step(result.contact_stream, events, result.contacts_added);
  }
  result.ray = world.cast_ray(JPH::RVec3(0.0f, 20.0f, 0.0f), JPH::Vec3(0.0f, -40.0f, 0.0f));
  result.state = world.snapshot();
  result.workers_started = world.worker_starts();
  result.published = publication.count;
  if (world.unresolved_removed_contacts() != 0 || world.contact_overflow() != 0) std::abort();
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
  const auto publication = world.publish_pending();
  const ray_hit after =
      world.cast_ray(JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f));
  const size_t overlap_after = world.overlap_point(JPH::RVec3::sZero());
  return {
      !before.body && overlap_before == 0,
      publication.status == physics_world::publish_status::published
          && publication.count == 1 && bool(after.body),
      overlap_after};
}

struct lifecycle_probe {
  bool failed_batch_invisible = false;
  bool retry_published = false;
  bool slot_reused_with_new_generation = false;
  bool stale_handle_rejected = false;
};

lifecycle_probe probe_lifecycle(const fixture_shapes& shapes) {
  physics_world world(shapes.catalog, 2, physics_world::mode::queries_only, 0);
  const body_handle first = world.queue_body(
      shapes.query_box, JPH::RVec3(-4.0f, 0.0f, 0.0f), JPH::EMotionType::Static);
  world.queue_body(
      shapes.query_box, JPH::RVec3(0.0f, 0.0f, 0.0f), JPH::EMotionType::Static);
  const body_handle excess = world.queue_body(
      shapes.query_box, JPH::RVec3(4.0f, 0.0f, 0.0f), JPH::EMotionType::Static);

  const auto refused = world.publish_pending();
  const bool invisible = !world.cast_ray(
      JPH::RVec3(0.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f)).body;
  if (!world.destroy_body(excess)) std::abort();
  const auto retry = world.publish_pending();

  if (!world.destroy_body(first)) std::abort();
  const bool stale_rejected = !world.is_alive(first) && !world.destroy_body(first);
  const body_handle replacement = world.queue_body(
      shapes.query_box, JPH::RVec3(4.0f, 0.0f, 0.0f), JPH::EMotionType::Static);
  const bool reused = replacement.value == first.value
      && replacement.generation != first.generation;
  const bool replacement_invisible = !world.cast_ray(
      JPH::RVec3(4.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f)).body;
  const auto replacement_commit = world.publish_pending();
  const ray_hit replacement_hit = world.cast_ray(
      JPH::RVec3(4.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, -6.0f, 0.0f));

  return {
      refused.status == physics_world::publish_status::body_capacity_exhausted
          && refused.count == 0 && invisible,
      retry.status == physics_world::publish_status::published && retry.count == 2,
      reused && replacement_invisible
          && replacement_commit.status == physics_world::publish_status::published
          && replacement_commit.count == 1 && replacement_hit.body == replacement,
      stale_rejected};
}

struct removed_contact_probe {
  bool attributed_after_destroy = false;
  bool no_unresolved_events = false;
};

removed_contact_probe probe_removed_contact(const fixture_shapes& shapes) {
  physics_world world(shapes.catalog, 4, physics_world::mode::simulation, 0);
  const body_handle floor = world.queue_body(
      shapes.floor, JPH::RVec3(0.0f, -1.0f, 0.0f), JPH::EMotionType::Static);
  const body_handle falling = world.queue_body(
      shapes.box, JPH::RVec3(0.0f, 0.4f, 0.0f), JPH::EMotionType::Dynamic);
  const auto publication = world.publish_pending();
  if (publication.status != physics_world::publish_status::published) std::abort();
  const std::vector<contact_event> first_step = world.step(1.0f / 60.0f);
  const bool contact_was_cached = std::any_of(
      first_step.begin(), first_step.end(), [](const contact_event& event) {
        return event.phase == contact_phase::added;
      });
  if (!world.destroy_body(falling)) std::abort();
  const std::vector<contact_event> second_step = world.step(1.0f / 60.0f);
  const bool attributed = std::any_of(
      second_step.begin(), second_step.end(), [&](const contact_event& event) {
        return event.phase == contact_phase::removed
            && ((event.first == floor && event.second == falling)
                || (event.first == falling && event.second == floor));
      });
  return {
      contact_was_cached && attributed,
      world.unresolved_removed_contacts() == 0 && world.contact_overflow() == 0};
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
  const lifecycle_probe lifecycle = probe_lifecycle(shapes);
  const removed_contact_probe removed_contact = probe_removed_contact(shapes);
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
            << "  contacts.canonical_hash=0x" << std::hex
            << hash_bytes(threaded.contact_stream) << std::dec << '\n'
            << "  ray.hit_handle=" << threaded.ray.body.value
            << " fraction=" << threaded.ray.fraction << '\n'
            << "  query_only.before_commit="
            << (queries.invisible_before_commit ? "invisible" : "VISIBLE")
            << " after_commit=" << (queries.visible_after_commit ? "visible" : "MISSING")
            << " point_overlaps=" << queries.overlap_count << '\n'
            << "  lifecycle.rollback="
            << (lifecycle.failed_batch_invisible ? "invisible" : "LEAKED")
            << " retry=" << (lifecycle.retry_published ? "published" : "FAILED")
            << " reuse="
            << (lifecycle.slot_reused_with_new_generation ? "new-generation" : "STALE")
            << " stale=" << (lifecycle.stale_handle_rejected ? "rejected" : "ACCEPTED") << '\n'
            << "  contacts.removed_after_destroy="
            << (removed_contact.attributed_after_destroy ? "attributed" : "LOST")
            << " unresolved=" << (removed_contact.no_unresolved_events ? 0 : 1) << '\n'
            << "  math.sin_cos_hash=0x" << std::hex << math.signature << std::dec
            << " max_unit_circle_error=" << math.max_unit_circle_error
            << " matches_jolt=" << (math.matches_jolt ? "yes" : "NO") << '\n';

  if (!threaded.ray.body || threaded.contacts_added == 0
      || threaded.published != size_t(opts.bodies) + 1
      || threaded.workers_started != opts.threads || !math.matches_jolt
      || !queries.invisible_before_commit || !queries.visible_after_commit
      || queries.overlap_count != 1 || !lifecycle.failed_batch_invisible
      || !lifecycle.retry_published || !lifecycle.slot_reused_with_new_generation
      || !lifecycle.stale_handle_rejected || !removed_contact.attributed_after_destroy
      || !removed_contact.no_unresolved_events) {
    std::fputs("PHY01: basic scene or worker startup check failed\n", stderr);
    return 1;
  }

  if (opts.verify) {
    const run_result serial = run_scene(opts, shapes, 0);
    const bool state_equal = serial.state == threaded.state;
    // Handles from different worlds intentionally do not compare equal; the
    // stable slots and hit fractions should still correspond.
    const bool ray_equal = serial.ray.body.value == threaded.ray.body.value
        && serial.ray.body.generation == threaded.ray.body.generation
        && std::bit_cast<uint32_t>(serial.ray.fraction)
            == std::bit_cast<uint32_t>(threaded.ray.fraction);
    const bool contacts_equal = serial.contact_stream == threaded.contact_stream;
    std::cout << "  verify.serial_vs_threaded_state=" << (state_equal ? "bit-exact" : "DIFF")
              << " ray=" << (ray_equal ? "bit-exact" : "DIFF")
              << " contacts=" << (contacts_equal ? "bit-exact" : "DIFF") << '\n';
    if (!state_equal || !ray_equal || !contacts_equal) return 1;
  }

  return 0;
}
