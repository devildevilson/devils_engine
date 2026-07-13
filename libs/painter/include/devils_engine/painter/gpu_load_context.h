#ifndef DEVILS_ENGINE_PAINTER_GPU_LOAD_CONTEXT_H
#define DEVILS_ENGINE_PAINTER_GPU_LOAD_CONTEXT_H

namespace devils_engine {
namespace painter {

struct assets_base;
struct graphics_base;

// Контекст GPU-стороны: поток рендера передаёт его в load_warm/unload_hot ресурса через handle
// (warm↔hot исполняет рендер, а не поток ассетов — см. demiurg::resource_loader).
struct gpu_load_context {
  assets_base* assets;
  graphics_base* base;
};

} // namespace painter
} // namespace devils_engine

#endif
