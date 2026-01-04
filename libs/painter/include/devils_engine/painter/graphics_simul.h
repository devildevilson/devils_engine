#ifndef DEVILS_ENGINE_PAINTER_GRAPHICS_SIMUL_H
#define DEVILS_ENGINE_PAINTER_GRAPHICS_SIMUL_H

#include <cstdint>
#include <cstddef>

#include "devils_engine/simul/interface.h"

#include "graphics_base.h"

namespace devils_engine {
namespace painter {
class graphics_simul : public simul::interface {
public:
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

private:
  // окно? чет мне кажется что не здесь
  graphics_base base;
  // что тут будет? графический кеш
  // ну и запуск всего добра
  // наверное алгоритм запуска вообще вынесем 
};
}
}

#endif