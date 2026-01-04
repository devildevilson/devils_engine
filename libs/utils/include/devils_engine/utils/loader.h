#ifndef DEVILS_ENGINE_UTILS_LOADER_H
#define DEVILS_ENGINE_UTILS_LOADER_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include "devils_engine/thread/lock.h"
#include "load_stage.h"

namespace devils_engine {
namespace utils {

// надо упаковать весь процесс загрузки в эти классы
// их можно расположить в цепочку которая буфет считывать данные с семафоры
// в том числе можно так же оформить дозагрузку контента во время игры
// по максимуму использовать лоадеры в разных потоках
// лоадеры поди должны представлять собой очереди 
// где текущий считает состояние предыдущего и только тогда запустит вычисления

class loader : public load_stage, public thread::semaphore_interface {
public:
  static constexpr size_t max_waiters = 8;

  loader(std::string name) noexcept;
  virtual ~loader() noexcept = default;
  
  // NOT THREAD SYNC
  template <typename T, typename... Args>
  T* add(Args&&... args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto p = ptr.get();
    _stages.push_back(std::move(ptr));
    return p;
  }

  // NOT THREAD SYNC
  // когда функция закончит вычисления для этого loader, 
  // то он разрушится и естественно указателю тоже придет конец
  // возвращать отсюда булевы флаги?
  void add_waiter(thread::semaphore_interface* inter);

  // _tp and _counter is changing
  void process() const override;

  size_t counter() const noexcept;
  size_t size() const noexcept;
  bool finished() const noexcept;
  std::string_view stage_name() const noexcept;
  tp start_time() const noexcept;

  void reset() override;
  thread::semaphore_state::values state() const override;
  bool wait_until(tp t, const size_t tolerance_in_ns = 1) const override;
protected:
  // unfortunately looks very ugly
  mutable std::atomic<size_t> _counter;
  mutable std::atomic<size_t> _tp;
  std::vector<std::unique_ptr<load_stage>> _stages;
  size_t waiters_count;
  thread::semaphore_interface* waiters[max_waiters];
};

class loader2 {
public:
  using lock = std::unique_lock<std::mutex>;

  std::string name;
  
  loader2(std::string name) noexcept;
  ~loader2() noexcept = default;
  
  template <typename T, typename... Args>
  T* add(Args&&... args) {
    lock l(_mutex);
    _stages.push_back(std::make_unique<T>(std::forward<Args>(args)...));
  }

  void process();

  size_t counter() const noexcept;
  size_t size() const noexcept;
  bool finished() const noexcept;
  std::string_view stage_name() const noexcept;
protected:
  mutable std::mutex _mutex;
  load_stage* _stage;
  size_t _counter;
  std::vector<std::unique_ptr<load_stage>> _stages;
};
}
}

#endif