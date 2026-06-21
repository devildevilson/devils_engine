#include "assets_system.h"

#include <devils_engine/thread/atomic_pool.h>

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// тут что? система звуков + кеш?
struct assets_simulation_init {
  thread::atomic_pool* pool;
};

assets_simulation::assets_simulation(const size_t frame_time) noexcept : simul::advancer(frame_time) {}
assets_simulation::~assets_simulation() noexcept = default;

void assets_simulation::init() {
  container.reset(new assets_simulation_init);
  container->pool = nullptr;
}

bool assets_simulation::stop_predicate() const { return false; }

void assets_simulation::update(const size_t time) {
  // тут что? принимаем заявки на загрузку ресурсов
  // желательно чтобы заявка была сразу как можно больше
  // после чего работаем в двух режимах:
  // один основной поток или владеет тредпулом

  if (container->pool != nullptr) {
    // закидываем задачи в тредпул + берем задачу для себя
    // нужно как то составить список того что сейчас загружается
    // возвращаем пул после того как все выполнили
  } else {
    // берем по одной задаче
  }

  // тут в плане логики все довольно легко,
  // нужно убедиться что ресурсы доступны в остальных системах только в состоянии ready
  // загрузка может зависить от загрузки зависимого ресурса
  // как минимум карта обращается к текстуре у которой уже должен быть известен слот

  //utils::info("Assets loop");
}

assets_actor* assets_simulation::get_actor() { return &actor; }

}
}
