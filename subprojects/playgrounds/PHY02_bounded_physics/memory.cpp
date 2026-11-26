#include "memory.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

using namespace devils_engine;

namespace {

struct counters {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> bytes{0};

  void add(size_t size) noexcept {
    calls.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(size, std::memory_order_relaxed);
  }

  phy02::allocation_count sample() const noexcept {
    return {calls.load(std::memory_order_relaxed), bytes.load(std::memory_order_relaxed)};
  }
};

constinit counters engine_allocations;
constinit counters jolt_allocations;
thread_local bool in_jolt = false;
thread_local bool solver_limit_expected = false;
JPH::AllocateFunction default_allocate = nullptr;
JPH::ReallocateFunction default_reallocate = nullptr;
JPH::AlignedAllocateFunction default_aligned_allocate = nullptr;

void* cpp_allocate(size_t size, size_t alignment) {
  const size_t actual = std::max(size, size_t{1});
  const size_t overhead = sizeof(void*) + alignment - 1;
  if (actual > std::numeric_limits<size_t>::max() - overhead) throw std::bad_alloc{};
  const size_t block_size = actual + overhead;
  void* block = std::malloc(block_size);
  if (!block) throw std::bad_alloc{};
  void* result = static_cast<char*>(block) + sizeof(void*);
  size_t space = block_size - sizeof(void*);
  std::align(alignment, actual, result, space);
  static_cast<void**>(result)[-1] = block;
  (in_jolt ? jolt_allocations : engine_allocations).add(size);
  return result;
}

void cpp_free(void* ptr) noexcept {
  if (ptr) std::free(static_cast<void**>(ptr)[-1]);
}

void* jolt_allocate(size_t size) {
  jolt_allocations.add(size);
  return default_allocate(size);
}

void* jolt_reallocate(void* ptr, size_t previous, size_t size) {
  jolt_allocations.add(size);
  return default_reallocate(ptr, previous, size);
}

void* jolt_allocate_aligned(size_t size, size_t alignment) {
  jolt_allocations.add(size);
  return default_aligned_allocate(size, alignment);
}

void trace(const char* format, ...) {
  std::va_list args;
  va_start(args, format);
  std::vfprintf(stderr, format, args);
  va_end(args);
  std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertion_failed(const char* expression, const char* message, const char* file, JPH::uint line) {
  if (solver_limit_expected && std::string_view(file).ends_with("PhysicsSystem.cpp") &&
      std::string_view(expression) == "errors == EPhysicsUpdateError::None") return false;
  std::fprintf(stderr, "PHY02 Jolt assertion %s:%u: %s (%s)\n", file, line, expression, message ? message : "");
  return true;
}
#endif

} // namespace

void* operator new(size_t size) { return cpp_allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__); }
void* operator new[](size_t size) { return ::operator new(size); }
void* operator new(size_t size, std::align_val_t align) { return cpp_allocate(size, size_t(align)); }
void* operator new[](size_t size, std::align_val_t align) { return ::operator new(size, align); }
void operator delete(void* ptr) noexcept { cpp_free(ptr); }
void operator delete[](void* ptr) noexcept { cpp_free(ptr); }
void operator delete(void* ptr, size_t) noexcept { cpp_free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { cpp_free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { cpp_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { cpp_free(ptr); }
void operator delete(void* ptr, size_t, std::align_val_t) noexcept { cpp_free(ptr); }
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept { cpp_free(ptr); }

namespace devils_engine::phy02 {

allocation_sample allocation_totals() noexcept {
  return {engine_allocations.sample(), jolt_allocations.sample()};
}

allocation_sample allocation_delta(allocation_sample before, allocation_sample after) noexcept {
  return {{after.engine.calls - before.engine.calls, after.engine.bytes - before.engine.bytes},
          {after.jolt.calls - before.jolt.calls, after.jolt.bytes - before.jolt.bytes}};
}

void mark_jolt_thread() noexcept { in_jolt = true; }
backend_allocations::backend_allocations() noexcept : previous_(in_jolt) { in_jolt = true; }
backend_allocations::~backend_allocations() { in_jolt = previous_; }

expect_solver_limit::expect_solver_limit() noexcept : previous_(solver_limit_expected) { solver_limit_expected = true; }
expect_solver_limit::~expect_solver_limit() { solver_limit_expected = previous_; }

jolt_runtime::jolt_runtime() {
  JPH::RegisterDefaultAllocator();
  default_allocate = JPH::Allocate;
  default_reallocate = JPH::Reallocate;
  default_aligned_allocate = JPH::AlignedAllocate;
  JPH::Allocate = jolt_allocate;
  JPH::Reallocate = jolt_reallocate;
  JPH::AlignedAllocate = jolt_allocate_aligned;
  JPH::Trace = trace;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = assertion_failed;
#endif
  JPH::Factory::sInstance = new JPH::Factory;
  JPH::RegisterTypes();
}

jolt_runtime::~jolt_runtime() {
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
  JPH::Trace = nullptr;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = nullptr;
#endif
  JPH::Allocate = default_allocate;
  JPH::Reallocate = default_reallocate;
  JPH::AlignedAllocate = default_aligned_allocate;
}

step_scratch::step_scratch(size_t capacity) : arena_(capacity) {}

void* step_scratch::Allocate(JPH::uint size) {
  if (!arena_.CanAllocate(size)) {
    std::fprintf(stderr, "PHY02 solver scratch: used %zu + request %u bytes exceeds limit %zu\n",
                 arena_.GetUsage(), size, arena_.GetSize());
    std::fflush(stderr);
    std::abort();
  }
  void* result = arena_.Allocate(size);
  peak_ = std::max(peak_, arena_.GetUsage());
  return result;
}

void step_scratch::Free(void* address, JPH::uint size) { arena_.Free(address, size); }

} // namespace devils_engine::phy02
