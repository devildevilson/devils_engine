#pragma once

#include "contacts.h"

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <chrono>
#include <memory>

namespace devils_engine::phy02 {

constexpr JPH::ObjectLayer fixed_layer = 0;
constexpr JPH::ObjectLayer dynamic_layer = 1;
constexpr JPH::BroadPhaseLayer fixed_broad{0};
constexpr JPH::BroadPhaseLayer dynamic_broad{1};

struct body_handle {
  // Owner-local slot in low bits, generation in high bits.
  uint64_t key = 0;
  uint64_t world = 0;
  explicit operator bool() const noexcept { return key != 0; }
};

enum class lookup_mode { linear, index, user_data };
enum class query_kind { ray, overlap };
enum class query_status { ok, hit_capacity, candidate_capacity, invalid_body };

struct hit {
  JPH::BodyID id;
  uint64_t key = 0;
  uint32_t subshape = 0;
  float fraction = 0;

  auto rank() const noexcept { return std::tuple(fraction, key, subshape); }
};

struct query_result {
  std::span<hit> storage;
  size_t count = 0;
  size_t candidates = 0;
  size_t offered = 0;
  query_status status = query_status::ok;

  std::span<const hit> hits() const noexcept { return storage.first(count); }
};

struct query {
  query_kind kind;
  JPH::RVec3 origin;
  JPH::Vec3 delta;
  const JPH::Shape* shape = nullptr;
};

template<class Base>
class bounded_collector final : public Base {
public:
  bounded_collector(query_result& result, size_t candidate_limit)
    : result_(result), candidate_limit_(candidate_limit) {}

  void OnBody(const JPH::Body&) override {
    ++result_.candidates;
    if (result_.candidates > candidate_limit_) stop(query_status::candidate_capacity);
  }

  void AddHit(const typename Base::ResultType& value) override {
    if (result_.status != query_status::ok) return;
    ++result_.offered;
    if (result_.count == result_.storage.size()) {
      stop(query_status::hit_capacity);
      return;
    }
    if constexpr (std::is_same_v<Base, JPH::CastRayCollector>) {
      const float fraction = value.mFraction == 0 ? 0.0f : value.mFraction;
      result_.storage[result_.count++] = {value.mBodyID, 0, value.mSubShapeID2.GetValue(), fraction};
    } else {
      result_.storage[result_.count++] = {value.mBodyID2, 0, value.mSubShapeID2.GetValue(), 0};
    }
  }

private:
  void stop(query_status status) {
    result_.status = status;
    this->ForceEarlyOut();
  }

  query_result& result_;
  size_t candidate_limit_;
};

struct limits {
  size_t bodies = 128;
  size_t commands = 128;
  size_t pairs = 2048;
  size_t events = 4096;
  size_t scratch = 8 * 1024 * 1024;
  uint32_t solver_pairs = 4096;
  uint32_t solver_contacts = 4096;
};

struct step_result {
  JPH::EPhysicsUpdateError solver = JPH::EPhysicsUpdateError::None;
  contact_status contacts = contact_status::ok;
  double update_us = 0;
  double gather_us = 0;
  double seal_us = 0;
  bool good() const noexcept {
    return solver == JPH::EPhysicsUpdateError::None && contacts == contact_status::ok;
  }
};

inline double micros(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::micro>(duration).count();
}

class world {
public:
  world(const limits& cap, pair_storage pairs, event_storage events, uint32_t workers,
        bool shared, thread::atomic_pool& pool)
    : cap_(cap), events_(cap.pairs, cap.events, pairs, events), scratch_(cap.scratch),
      slots_(cap.bodies), reverse_(cap.bodies) {
    free_.reserve(cap.bodies);
    pending_.reserve(cap.commands);
    batch_.reserve(cap.commands);
    for (auto i = cap.bodies; i > 0; --i) free_.push_back(uint32_t(i - 1));
    pair_filter_.EnableCollision(fixed_layer, dynamic_layer);
    pair_filter_.EnableCollision(dynamic_layer, dynamic_layer);
    broad_.MapObjectToBroadPhaseLayer(fixed_layer, fixed_broad);
    broad_.MapObjectToBroadPhaseLayer(dynamic_layer, dynamic_broad);
    object_filter_ = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broad_, 2, pair_filter_, 2);
    system_.Init(uint32_t(cap.bodies), 0, cap.solver_pairs, cap.solver_contacts,
                 broad_, *object_filter_, pair_filter_);
    system_.SetContactListener(&events_);
    if (shared) {
      shared_ = std::make_unique<shared_jobs>(pool, workers, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    } else {
      stock_ = std::make_unique<JPH::JobSystemThreadPool>();
      stock_->SetThreadInitFunction([](int i) { contact_lane = uint32_t(i + 1); mark_jolt_thread(); });
      stock_->Init(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, int(workers));
    }
  }

  ~world() {
    auto& bodies = system_.GetBodyInterface();
    for (const auto& slot : slots_) {
      if (slot.state == state::published) {
        bodies.RemoveBody(slot.id);
        bodies.DestroyBody(slot.id);
      }
    }
  }

  enum class command_status { ok, command_capacity, body_capacity, stale };
  struct admission {
    command_status status;
    body_handle handle;
  };

  admission queue(const JPH::Shape* shape, JPH::RVec3Arg position,
                  JPH::EMotionType motion, bool allow_sleep = true) {
    if (pending_.size() == cap_.commands) return {command_status::command_capacity, {}};
    if (free_.empty()) return {command_status::body_capacity, {}};
    const uint32_t index = free_.back();
    free_.pop_back();
    auto& slot = slots_[index];
    slot.state = state::pending;
    const uint64_t key = (uint64_t(slot.generation) << 32) | (index + 1);
    pending_.push_back({shape, position, motion, index, key, allow_sleep});
    command_peak_ = std::max(command_peak_, pending_.size());
    return {command_status::ok, {key, token_}};
  }

  command_status publish() {
    if (pending_.empty()) return command_status::ok;
    batch_.clear();
    auto& bodies = system_.GetBodyInterface();
    for (const auto& item : pending_) {
      const auto layer = item.motion == JPH::EMotionType::Static ? fixed_layer : dynamic_layer;
      JPH::BodyCreationSettings settings(item.shape, item.position, JPH::Quat::sIdentity(), item.motion, layer);
      settings.mUserData = item.key;
      settings.mAllowSleeping = item.allow_sleep;
      auto* body = bodies.CreateBody(settings);
      if (!body) {
        for (auto id : batch_) bodies.DestroyBody(id);
        for (const auto& queued : pending_) slots_[queued.index].id = {};
        batch_.clear();
        return command_status::body_capacity;
      }
      slots_[item.index].id = body->GetID();
      batch_.push_back(body->GetID());
    }
    const auto prepared = bodies.AddBodiesPrepare(batch_.data(), int(batch_.size()));
    bodies.AddBodiesFinalize(batch_.data(), int(batch_.size()), prepared, JPH::EActivation::Activate);
    // Prepare may permute batch_, but the slot received its ID directly from CreateBody.
    for (const auto& item : pending_) {
      auto& slot = slots_[item.index];
      slot.state = state::published;
      reverse_[slot.id.GetIndex()] = {slot.id, item.key};
    }
    live_ += pending_.size();
    body_peak_ = std::max(body_peak_, live_);
    pending_.clear();
    batch_.clear();
    return command_status::ok;
  }

  command_status remove(body_handle handle) {
    auto* slot = find(handle);
    if (!slot) return command_status::stale;
    const auto index = uint32_t(handle.key) - 1;
    if (slot->state == state::pending) {
      const auto found = std::find_if(pending_.begin(), pending_.end(),
                                      [index](const auto& item) { return item.index == index; });
      if (found == pending_.end()) utils::error{}("PHY02 remove: missing pending slot {}", index);
      pending_.erase(found);
    } else {
      auto& bodies = system_.GetBodyInterface();
      bodies.RemoveBody(slot->id);
      bodies.DestroyBody(slot->id);
      reverse_[slot->id.GetIndex()] = {};
      --live_;
    }
    slot->id = {};
    if (slot->generation != UINT32_MAX) {
      slot->state = state::free;
      ++slot->generation;
      free_.push_back(index);
    } else {
      slot->state = state::retired;
    }
    return command_status::ok;
  }

  bool published(body_handle handle) const {
    const auto* slot = find(handle);
    return slot && slot->state == state::published;
  }

  bool active(body_handle handle) const {
    return published(handle) && system_.GetBodyInterface().IsActive(find(handle)->id);
  }

  void impulse(body_handle handle, JPH::Vec3Arg amount) {
    if (!published(handle)) utils::error{}("PHY02 impulse: unpublished body key {}", handle.key);
    system_.GetBodyInterface().AddImpulse(find(handle)->id, amount);
  }

  void optimize() { system_.OptimizeBroadPhase(); }

  step_result step() {
    if (poisoned_) utils::error{}("PHY02 step: world failed before publication");
    published_step_ = false;
    step_result result;
    const auto begin = std::chrono::steady_clock::now();
    if (shared_) shared_->start();
    {
      const backend_allocations scope;
      JPH::JobSystem* jobs = shared_ ? static_cast<JPH::JobSystem*>(shared_.get()) : stock_.get();
      result.solver = system_.Update(1.0f / 60.0f, 1, &scratch_, jobs);
    }
    if (shared_) shared_->finish();
    const auto updated = std::chrono::steady_clock::now();
    if (result.solver == JPH::EPhysicsUpdateError::None) {
      result.contacts = events_.gather();
      const auto gathered = std::chrono::steady_clock::now();
      if (result.contacts == contact_status::ok) result.contacts = events_.seal();
      result.gather_us = micros(gathered - updated);
      result.seal_us = micros(std::chrono::steady_clock::now() - gathered);
    }
    result.update_us = micros(updated - begin);
    published_step_ = result.good();
    poisoned_ = !result.good();
    return result;
  }

  std::span<const contact_event> contacts() const noexcept {
    return published_step_ ? events_.output() : std::span<const contact_event>{};
  }

  // Explicit coarse query for callers that need candidate bodies. The normal
  // ray/overlap path uses Jolt's own traversal and doesn't repeat this query.
  query_result candidates(const JPH::AABox& bounds, std::span<hit> storage) const {
    class collector final : public JPH::CollideShapeBodyCollector {
    public:
      explicit collector(query_result& output) : output_(output) {}

      void AddHit(const JPH::BodyID& id) override {
        ++output_.candidates;
        if (output_.count == output_.storage.size()) {
          output_.status = query_status::candidate_capacity;
          ForceEarlyOut();
          return;
        }
        output_.storage[output_.count++] = {id};
      }

    private:
      query_result& output_;
    };

    if (poisoned_) utils::error{}("PHY02 coarse query: world failed before publication");
    query_result result{storage};
    collector collect(result);
    {
      const backend_allocations scope;
      system_.GetBroadPhaseQuery().CollideAABox(bounds, collect);
    }
    if (result.status != query_status::ok) {
      result.count = 0;
      return result;
    }
    for (auto& item : storage.first(result.count)) {
      item.key = resolve(item.id, lookup_mode::index);
      if (!item.key) {
        result.count = 0;
        result.status = query_status::invalid_body;
        return result;
      }
    }
    const auto data = storage.first(result.count);
    utils::deterministic_sort(data.begin(), data.end(),
      [](const hit& a, const hit& b) { return a.key < b.key; });
    return result;
  }

  query_result run(const query& cmd, std::span<hit> storage, size_t candidates, lookup_mode lookup) const {
    query_result result{storage};
    if (poisoned_) utils::error{}("PHY02 query: world failed before publication");
    {
      const backend_allocations scope;
      const auto& narrow = system_.GetNarrowPhaseQuery();
      if (cmd.kind == query_kind::ray) {
        bounded_collector<JPH::CastRayCollector> collector(result, candidates);
        narrow.CastRay(JPH::RRayCast(cmd.origin, cmd.delta), {}, collector);
      } else {
        bounded_collector<JPH::CollideShapeCollector> collector(result, candidates);
        narrow.CollideShape(cmd.shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(cmd.origin), {},
                            cmd.origin, collector);
      }
    }
    for (size_t i = 0; i < result.count && result.status == query_status::ok; ++i) {
      auto& item = storage[i];
      item.key = resolve(item.id, lookup);
      if (!item.key) result.status = query_status::invalid_body;
    }
    if (result.status != query_status::ok) result.count = 0;
    const auto data = storage.first(result.count);
    utils::deterministic_sort(data.begin(), data.end(),
      [](const hit& a, const hit& b) { return a.rank() < b.rank(); });
    // Narrow phase can produce several contact points for one exact shape pair.
    result.count = size_t(std::unique(data.begin(), data.end(),
      [](const hit& a, const hit& b) { return a.rank() == b.rank(); }) - data.begin());
    return result;
  }

  static size_t objects(std::span<const hit> hits, std::span<uint64_t> out) {
    // Callers provide at least as many slots as hits. Build a list sorted by stable
    // key for a linear enter/exit diff, independently of ray distance ordering.
    for (size_t i = 0; i < hits.size(); ++i) out[i] = hits[i].key;
    const auto working = out.first(hits.size());
    utils::deterministic_sort(working.begin(), working.end(), std::less{});
    return size_t(std::unique(working.begin(), working.end()) - working.begin());
  }

  void append_state(std::vector<uint64_t>& trace) const {
    const auto& bodies = system_.GetBodyInterface();
    for (size_t i = 0; i < slots_.size(); ++i) {
      const auto& slot = slots_[i];
      if (slot.state != state::published) continue;
      trace.push_back((uint64_t(slot.generation) << 32) | (i + 1));
      const auto p = bodies.GetPosition(slot.id);
      trace.push_back(std::bit_cast<uint64_t>(double(p.GetX())));
      trace.push_back(std::bit_cast<uint64_t>(double(p.GetY())));
      trace.push_back(std::bit_cast<uint64_t>(double(p.GetZ())));
      const auto q = bodies.GetRotation(slot.id);
      const auto v = bodies.GetLinearVelocity(slot.id);
      const auto w = bodies.GetAngularVelocity(slot.id);
      for (auto x : {q.GetX(), q.GetY(), q.GetZ(), q.GetW(), v.GetX(), v.GetY(), v.GetZ(),
                     w.GetX(), w.GetY(), w.GetZ()}) trace.push_back(std::bit_cast<uint32_t>(x));
      trace.push_back(bodies.IsActive(slot.id));
    }
  }

  size_t body_peak() const noexcept { return body_peak_; }
  const limits& capacity() const noexcept { return cap_; }
  size_t command_peak() const noexcept { return command_peak_; }
  size_t scratch_peak() const noexcept { return scratch_.peak(); }
  bool scratch_empty() const noexcept { return scratch_.empty(); }
  size_t event_peak() const noexcept { return events_.event_peak(); }
  size_t pair_peak() const noexcept { return events_.pair_peak(); }
  size_t pair_bytes() const noexcept { return events_.pair_bytes(); }
  size_t queue_peak() const noexcept { return shared_ ? shared_->peak() : 0; }
  size_t reserved_bytes() const noexcept {
    return slots_.capacity() * sizeof(body_slot) + reverse_.capacity() * sizeof(reverse_entry)
      + free_.capacity() * sizeof(uint32_t) + pending_.capacity() * sizeof(command)
      + batch_.capacity() * sizeof(JPH::BodyID) + events_.reserved_bytes()
      + (shared_ ? shared_->capacity() * sizeof(void*) : 0);
  }

private:
  enum class state { free, pending, published, retired };
  struct body_slot {
    JPH::BodyID id;
    uint32_t generation = 1;
    enum state state = state::free;
  };
  struct reverse_entry {
    JPH::BodyID id;
    uint64_t key = 0;
  };
  struct command {
    JPH::ShapeRefC shape;
    JPH::RVec3 position;
    JPH::EMotionType motion;
    uint32_t index;
    uint64_t key;
    bool allow_sleep;
  };

  const body_slot* find(body_handle handle) const {
    if (handle.world != token_ || uint32_t(handle.key) == 0 || uint32_t(handle.key) > slots_.size()) return nullptr;
    const auto& slot = slots_[uint32_t(handle.key) - 1];
    return slot.generation == uint32_t(handle.key >> 32)
       && (slot.state == state::pending || slot.state == state::published) ? &slot : nullptr;
  }
  body_slot* find(body_handle handle) { return const_cast<body_slot*>(std::as_const(*this).find(handle)); }

  uint64_t resolve(JPH::BodyID id, lookup_mode lookup) const {
    switch (lookup) {
      case lookup_mode::linear:
        for (size_t i = 0; i < slots_.size(); ++i) {
          const auto& slot = slots_[i];
          if (slot.state == state::published && slot.id == id) return (uint64_t(slot.generation) << 32) | (i + 1);
        }
        return 0;
      case lookup_mode::index: {
        if (id.GetIndex() >= reverse_.size()) return 0;
        const auto& entry = reverse_[id.GetIndex()];
        return entry.id == id ? entry.key : 0;
      }
      case lookup_mode::user_data: {
        const auto key = system_.GetBodyInterface().GetUserData(id);
        const auto* slot = find({key, token_});
        return slot && slot->state == state::published && slot->id == id ? key : 0;
      }
    }
    return 0;
  }

  limits cap_;
  JPH::ObjectLayerPairFilterTable pair_filter_{2};
  JPH::BroadPhaseLayerInterfaceTable broad_{2, 2};
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> object_filter_;
  contact_log events_;
  step_scratch scratch_;
  JPH::PhysicsSystem system_;
  std::unique_ptr<shared_jobs> shared_;
  std::unique_ptr<JPH::JobSystemThreadPool> stock_;
  std::vector<body_slot> slots_;
  std::vector<reverse_entry> reverse_;
  std::vector<uint32_t> free_;
  std::vector<command> pending_;
  std::vector<JPH::BodyID> batch_;
  size_t live_ = 0;
  size_t body_peak_ = 0;
  size_t command_peak_ = 0;
  bool published_step_ = false;
  bool poisoned_ = false;
  inline static std::atomic<uint64_t> next_token_{1};
  const uint64_t token_ = next_token_.fetch_add(1, std::memory_order_relaxed);
};

} // namespace devils_engine::phy02
