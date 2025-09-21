#ifndef DEVILS_ENGINE_ACUMEN_COMMON_H
#define DEVILS_ENGINE_ACUMEN_COMMON_H

#include <cstddef>
#include <cstdint>
#include <bitset>
#include <string>
#include <functional>

#ifndef DEVILS_ENGINE_ACUMEN_STATE_SIZE
#  define DEVILS_ENGINE_ACUMEN_STATE_SIZE 256
#endif

#define DEVILS_ENGINE_ACUMEN_EPSILON 0.0000001

namespace devils_engine {
namespace acumen {
// динамический сет?
using state = std::bitset<DEVILS_ENGINE_ACUMEN_STATE_SIZE>;

struct state_metric {
  using func_t = std::function<int32_t(const void*)>; // error when less than zero

  std::string name;
  double weight; // вес наверное нужен только для того чтобы сортировать goal
  func_t compute_func;

  state_metric() noexcept;
  state_metric(std::string name, func_t f) noexcept;
  state_metric(std::string name, const double weight, func_t f) noexcept;
  bool compute(const void* p) const;
};

struct scoped_state {
  state handle;
  state mask;

  scoped_state() noexcept = default;
  scoped_state(const state& handle) noexcept;
  scoped_state(const state& handle, const state& mask) noexcept;
  void set(const size_t& index, const bool val) noexcept;
  void unset(const size_t& index) noexcept;
  bool get(const size_t& index) const noexcept;
  state compute() const noexcept;

  bool check(const state& s) const noexcept;
  state apply(const state& s) const noexcept;
  std::tuple<size_t, size_t> count_similars(const state& s) const noexcept;
  double compute_variance_norm(const state& s) const noexcept;
};

struct goal {
  std::string name;
  scoped_state requirements; // validity of the state
  scoped_state goal;         //  цель достигнута если гоал - часть стейта
};

struct action {
  using func_t = std::function<int64_t(void*)>;

  std::string name;
  scoped_state requirements; // must be valid thru action? bold statement
  scoped_state next_state;   // action successfully end when next_state becomes part of the state
  scoped_state weight_state; // priority
  func_t action_func;

  action() noexcept;
  action(std::string name, scoped_state requirements, scoped_state next_state, scoped_state weight_state, func_t f) noexcept;
  double compute_weight(const state& current_state) const noexcept;
};
}
}

#endif