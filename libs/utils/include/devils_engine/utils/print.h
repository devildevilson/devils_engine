#pragma once

#include <print>
#include <utility>

namespace devils_engine::utils {
inline void print_detail() {}

template <typename Arg, typename... Args>
void print_detail(Arg&& arg, Args&&... args) {
  //std::cout << " " << arg;
  std::print(" {}", std::forward<Arg>(arg));
  print_detail(std::forward<Args>(args)...);
}

inline void print() {}

template <typename Arg, typename... Args>
void print(Arg&& arg, Args&&... args) {
  //std::cout << arg;
  std::print("{}", std::forward<Arg>(arg));
  print_detail(std::forward<Args>(args)...);
}

template <typename... Args>
void println(Args&&... args) {
  print(std::forward<Args>(args)...);
  //std::cout << "\n";
  std::print("\n");
}

} // namespace devils_engine::utils
