#ifndef DEVILS_ENGINE_RESOLVE_RUNNER_H
#define DEVILS_ENGINE_RESOLVE_RUNNER_H

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <type_traits>
#include <vector>

#include "devils_engine/thread/atomic_pool.h"
#include "work.h"

namespace devils_engine {
namespace resolve {

template <work_record Item>
class target_groups {
public:
  struct group_view {
    entity_key target = invalid_entity;
    size_t begin = 0;
    size_t end = 0;

    size_t size() const noexcept {
      return end - begin;
    }
  };

  void build(const std::span<const Item> records) {
    order_.resize(records.size());
    std::iota(order_.begin(), order_.end(), size_t{0});
    std::sort(order_.begin(), order_.end(), [&](const size_t lhs, const size_t rhs) {
      const auto& a = records[lhs].header;
      const auto& b = records[rhs].header;
      if (a.target != b.target) return a.target < b.target;
      return a.id < b.id;
    });

    groups_.clear();
    size_t begin = 0;
    while (begin < order_.size()) {
      const entity_key target = records[order_[begin]].header.target;
      size_t end = begin + 1;
      while (end < order_.size() && records[order_[end]].header.target == target)
        ++end;
      groups_.push_back(group_view{target, begin, end});
      begin = end;
    }
  }

  size_t group_count() const noexcept {
    return groups_.size();
  }

  const group_view& group(const size_t index) const {
    return groups_.at(index);
  }

  const Item& item(const std::span<const Item> records,
                   const size_t ordered_index) const {
    return records[order_.at(ordered_index)];
  }

private:
  std::vector<size_t> order_;
  std::vector<group_view> groups_;
};

// Serial reference path used for correctness tests and small batches. It has exactly the same
// target-group and within-target order as the MT path.
template <work_record Item, typename Fn>
void run_target_groups_serial(const target_groups<Item>& groups,
                              const std::span<const Item> records,
                              Fn&& fn) {
  for (size_t i = 0; i < groups.group_count(); ++i) {
    const auto& group = groups.group(i);
    for (size_t j = group.begin; j < group.end; ++j) {
      fn(group.target, groups.item(records, j));
    }
  }
}

// Top-level resolver phase: one pool barrier for all independent target groups. A group is processed
// by exactly one task and remains serial internally. Fn must write only the supplied target (or owned
// output slots) and must not submit/wait nested pool work.
template <work_record Item, typename Fn>
void run_target_groups(thread::atomic_pool& pool,
                       const target_groups<Item>& groups,
                       const std::span<const Item> records,
                       Fn&& fn) {
  if (groups.group_count() == 0) return;
  using fn_type = std::remove_reference_t<Fn>;
  fn_type* fn_ptr = &fn;
  pool.distribute1(
    groups.group_count(),
    [](const size_t start, const size_t count,
       const target_groups<Item>* group_index,
       const std::span<const Item> values,
       fn_type* callback) {
      for (size_t i = start; i < start + count; ++i) {
        const auto& group = group_index->group(i);
        for (size_t j = group.begin; j < group.end; ++j) {
          (*callback)(group.target, group_index->item(values, j));
        }
      }
    },
    &groups, records, fn_ptr);
  pool.compute();
  pool.wait();
}

} // namespace resolve
} // namespace devils_engine

#endif
