#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>

#include <devils_script/container.h>
#include <devils_script/context.h>
#include <devils_script/system.h>

// Микробенчмарк: одна и та же логика нативной C++-функцией vs скомпилированным devils_script-скриптом
// (VM-исполнение на context). Интересна разница на release — накладные расходы диспетчеризации ds
// против прямого вызова. Горячий путь ds повторяет script_function::invoke: clear() + set_arg + process.

namespace {

struct bench_scope {
  double hunger = 0.0;
  double strength = 0.0;
  bool valid() const noexcept {
    return true;
  }
};

// ds-аксессоры полей скоупа (как в проекте — read_stat).
double acc_hunger(bench_scope s) {
  return s.hunger;
}
double acc_strength(bench_scope s) {
  return s.strength;
}

// нативные аналоги скриптов — прямой C++.
bool native_pred(bench_scope s) {
  return s.hunger > 0.5;
}
double native_num(bench_scope s) {
  return s.hunger * 2.0 + s.strength;
}
double native_big(bench_scope s) {
  return s.hunger * 2.0 + s.strength * 3.0 - s.hunger + s.strength;
}

volatile std::uint64_t g_sink = 0;

template <typename F>
void bench(const std::string_view name, const std::size_t iterations, F&& f) {
  std::uint64_t local = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    local += f(i);
  }
  const auto end = std::chrono::steady_clock::now();
  g_sink = local;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  const double ns_per = double(ns) / double(iterations);
  std::cout << "  " << name << ": " << ns_per << " ns/call  (checksum=" << local << ")\n";
}

} // namespace

int main() {
  namespace ds = devils_script;
  ds::system sys;
  sys.init_basic_functions();
  sys.init_math();
  sys.register_function<&acc_hunger>("hunger");
  sys.register_function<&acc_strength>("strength");

  const auto pred_c = sys.parse<bool, bench_scope>("pred", "hunger > 0.5");
  const auto num_c = sys.parse<double, bench_scope>("num", "hunger * 2 + strength");
  const auto big_c = sys.parse<double, bench_scope>("big", "hunger * 2 + strength * 3 - hunger + strength");

  ds::context vm; // переиспользуемый скретчпад (как per-worker vm в проекте)

  const auto scope_of = [](std::size_t i) {
    return bench_scope{double(i & 1023u) / 1024.0, double(i & 7u)}; // варьируем, чтобы не свернуть в константу
  };

  constexpr std::size_t N = 5'000'000;
  std::cout << "script_vs_native (N=" << N << " calls each)\n";

  std::cout << "[predicate  hunger > 0.5]\n";
  bench("native", N, [&](std::size_t i) {
    return std::uint64_t(native_pred(scope_of(i)));
  });
  bench("script", N, [&](std::size_t i) {
    vm.clear();
    vm.set_arg(0, scope_of(i));
    pred_c.process(&vm);
    return std::uint64_t(vm.get_return<bool>());
  });

  std::cout << "[number  hunger*2 + strength]\n";
  bench("native", N, [&](std::size_t i) {
    return std::uint64_t(native_num(scope_of(i)));
  });
  bench("script", N, [&](std::size_t i) {
    vm.clear();
    vm.set_arg(0, scope_of(i));
    num_c.process(&vm);
    return std::uint64_t(vm.get_return<double>());
  });

  std::cout << "[number-big  hunger*2 + strength*3 - hunger + strength]\n";
  bench("native", N, [&](std::size_t i) {
    return std::uint64_t(native_big(scope_of(i)));
  });
  bench("script", N, [&](std::size_t i) {
    vm.clear();
    vm.set_arg(0, scope_of(i));
    big_c.process(&vm);
    return std::uint64_t(vm.get_return<double>());
  });

  return 0;
}
