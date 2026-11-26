#pragma once

#include "memory.h"

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <devils_engine/thread/atomic_pool.h>
#include <devils_engine/utils/error.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace devils_engine::phy02 {

inline thread_local uint32_t contact_lane = 0;

// One resident pump per participating worker during Update. Dynamic Jolt jobs
// queue into storage bounded by the live Job count, not into atomic_pool.
class shared_jobs final : public JPH::JobSystemWithBarrier {
public:
  shared_jobs(thread::atomic_pool& pool, uint32_t workers,
              JPH::uint max_jobs, JPH::uint max_barriers)
    : JPH::JobSystemWithBarrier(max_barriers), pool_(pool), workers_(workers), ready_(max_jobs) {
    jobs_.Init(max_jobs, max_jobs);
  }

  ~shared_jobs() override {
    if (open_ || size_ != 0) std::abort();
  }

  int GetMaxConcurrency() const override { return int(workers_) + 1; }

  JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color,
                          const JobFunction& fn, JPH::uint32 dependencies) override {
    const auto index = jobs_.ConstructObject(name, color, this, fn, dependencies);
    if (index == decltype(jobs_)::cInvalidObjectIndex) {
      std::fprintf(stderr, "PHY02 shared Jolt jobs: exhausted %zu slots\n", ready_.size());
      std::abort();
    }
    auto* job = &jobs_.Get(index);
    JPH::JobHandle handle(job);
    if (dependencies == 0) QueueJob(job);
    return handle;
  }

  void start() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    for (uint32_t i = 0; i < workers_; ++i) {
      pool_.submit([this, lane = i + 1] { pump(lane); });
    }
  }

  void finish() {
    {
      const std::lock_guard lock(mutex_);
      open_ = false;
    }
    ready_changed_.notify_all();
    pool_.wait(); // owner thread, after Jolt's own barrier and any independent fixture work
    if (size_ != 0) utils::error{}("PHY02 shared queue: {} jobs remain at finish", size_);
  }

  size_t peak() const noexcept { return peak_; }
  size_t capacity() const noexcept { return ready_.size(); }

protected:
  void FreeJob(Job* job) override { jobs_.DestructObject(job); }

  void QueueJob(Job* job) override { QueueJobs(&job, 1); }

  void QueueJobs(Job** jobs, JPH::uint count) override {
    if (workers_ == 0) return; // Jolt's barrier executes jobs on the calling thread.
    {
      const std::lock_guard lock(mutex_);
      if (count > ready_.size() - size_) {
        // Every queued pointer owns a distinct live Job. A producer inside
        // PhysicsSystem must never block for space in this queue.
        std::fprintf(stderr, "PHY02 shared ready queue: %zu + %u exceeds limit %zu\n",
                     size_, count, ready_.size());
        std::abort();
      }
      for (JPH::uint i = 0; i < count; ++i) {
        jobs[i]->AddRef();
        ready_[(head_ + size_) % ready_.size()] = jobs[i];
        ++size_;
      }
      peak_ = std::max(peak_, size_);
    }
    ready_changed_.notify_all();
  }

private:
  void pump(uint32_t lane) {
    contact_lane = lane;
    const backend_allocations scope;
    for (;;) {
      Job* job;
      {
        std::unique_lock lock(mutex_);
        ready_changed_.wait(lock, [this] { return size_ != 0 || !open_; });
        if (size_ == 0) break;
        job = ready_[head_];
        head_ = (head_ + 1) % ready_.size();
        --size_;
      }
      job->Execute(); // The barrier may already have executed it.
      job->Release();
    }
    contact_lane = 0;
  }

  thread::atomic_pool& pool_;
  uint32_t workers_;
  JPH::FixedSizeFreeList<Job> jobs_;
  std::vector<Job*> ready_;
  size_t head_ = 0;
  size_t size_ = 0;
  size_t peak_ = 0;
  bool open_ = false;
  std::mutex mutex_;
  std::condition_variable ready_changed_;
};

} // namespace devils_engine::phy02
