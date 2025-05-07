#ifndef DEVILS_ENGINE_VISAGE_DRAW_STAGE_H
#define DEVILS_ENGINE_VISAGE_DRAW_STAGE_H

#include <cstddef>
#include <cstdint>
#include "painter/primitives.h"

// отрисовка интерфейса как выглядит?

namespace devils_engine {
namespace visage {
struct interface_provider;

class interface_draw : public painter::sibling_stage {
public:
  inline interface_draw(const painter::pipeline_provider* pipe, const interface_provider* in) noexcept : pipe(pipe), in(in) {}
  void begin() override;
  void process(VkCommandBuffer buffer) override;
  void clear() override;
protected:
  const painter::pipeline_provider* pipe;
  const interface_provider* in;
};
}
}

#endif