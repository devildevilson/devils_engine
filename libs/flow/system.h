#ifndef DEVILS_ENGINE_FLOW_SYSTEM_H
#define DEVILS_ENGINE_FLOW_SYSTEM_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>

namespace devils_engine {
namespace flow {
struct state_t {
  using action_f = std::function<int32_t(void*)>;

  std::string name;
  action_f action;
  size_t time;
  const state_t* next;
  // спрайт ? это у нас явно ресурс и да и нет
  // как отличить спрайты которые могут поворачиваться?
  // скорее всего и так и сяк придется делать "списки" спрайтов
  // множество спрайтов будут сделаны просто зеркалкой какой нибудь стороны
  // 

  inline state_t() noexcept : time(0), next(nullptr) {}
};

// как это будет работать? у нас тут просто будет список стейтов
// оттуда мы возьмем ресурс со спрайтом
// если next == nullptr, то по идее меняем стейт юнита по таблице
class system {
public:

private:

};
}
}

#endif