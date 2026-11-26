#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>

#include <cstddef>
#include <cstdint>

namespace devils_engine::phy02 {

struct allocation_count {
  uint64_t calls = 0;
  uint64_t bytes = 0;
};

struct allocation_sample {
  allocation_count engine;
  allocation_count jolt;
};

allocation_sample allocation_totals() noexcept;
allocation_sample allocation_delta(allocation_sample before, allocation_sample after = allocation_totals()) noexcept;
void mark_jolt_thread() noexcept;

class backend_allocations {
public:
  backend_allocations() noexcept;
  ~backend_allocations();

private:
  bool previous_;
};

// The deliberate solver exhaustion fixture must inspect Update's flags even
// when Jolt asserts on those same flags in Debug.
class expect_solver_limit {
public:
  expect_solver_limit() noexcept;
  ~expect_solver_limit();

private:
  bool previous_;
};

class jolt_runtime {
public:
  jolt_runtime();
  ~jolt_runtime();
  jolt_runtime(const jolt_runtime&) = delete;
  jolt_runtime& operator=(const jolt_runtime&) = delete;
};

class step_scratch final : public JPH::TempAllocator {
public:
  explicit step_scratch(size_t capacity);
  void* Allocate(JPH::uint size) override;
  void Free(void* address, JPH::uint size) override;
  size_t peak() const noexcept { return peak_; }
  size_t capacity() const noexcept { return arena_.GetSize(); }
  bool empty() const noexcept { return arena_.IsEmpty(); }

private:
  JPH::TempAllocatorImpl arena_;
  size_t peak_ = 0;
};

} // namespace devils_engine::phy02
