#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include "devils_engine/utils/error.h"

namespace devils_engine::originator {
// Caller-owned intermediate values. One workspace per simultaneously executing call;
// independent immutable programs may use it sequentially on any worker.
struct column_workspace {
  static constexpr size_t lanes = 256;
  std::vector<double> values;
  std::vector<std::array<double, lanes>> computed;
  std::vector<std::array<double, lanes>> broadcast;
  std::vector<const double*> registers;
  size_t retained_bytes() const noexcept {
    return values.capacity() * sizeof(double) + (computed.capacity() + broadcast.capacity()) * sizeof(std::array<double, lanes>) + registers.capacity() * sizeof(const double*);
  }
  void release() { *this = {}; }
};

struct field_frame {
  column_workspace column;
  std::vector<const void*> candidates;
  std::vector<std::array<double, column_workspace::lanes>> slots;
  std::vector<double> combined;
  size_t retained_bytes() const noexcept {
    return column.retained_bytes() + candidates.capacity() * sizeof(const void*) + slots.capacity() * sizeof(std::array<double, column_workspace::lanes>) + combined.capacity() * sizeof(double);
  }
};

// Nested samplers get different stable frames: a child cannot invalidate its parent's
// candidate iterators, register pointers or noise columns.
class field_workspace {
public:
  field_workspace() = default;
  field_workspace(const field_workspace&) = delete;
  field_workspace& operator=(const field_workspace&) = delete;
  field_workspace(field_workspace&&) = default;
  field_workspace& operator=(field_workspace&&) = default;
  struct lease {
    field_workspace* owner;
    field_frame& frame;
    lease(const lease&) = delete;
    lease& operator=(const lease&) = delete;
    ~lease() { --owner->active_; }
    lease(field_workspace* owner_, field_frame& frame_) : owner(owner_), frame(frame_) {}
  };
  lease acquire() {
    if (active_ == frames_.size()) frames_.push_back(std::make_unique<field_frame>());
    return lease(this, *frames_[active_++]);
  }
  size_t retained_bytes() const noexcept {
    size_t bytes = frames_.capacity() * sizeof(std::unique_ptr<field_frame>) + frames_.size() * sizeof(field_frame);
    for (const auto& frame : frames_) bytes += frame->retained_bytes();
    return bytes;
  }
  void release() {
    if (active_ != 0) utils::error{}("originator field workspace: cannot release {} active frames", active_);
    decltype(frames_)().swap(frames_);
  }
private:
  std::vector<std::unique_ptr<field_frame>> frames_;
  size_t active_ = 0;
};
} // namespace devils_engine::originator
