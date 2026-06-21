#ifndef TILE_FRONTIER_CORE_GPU_LOAD_CONTEXT_H
#define TILE_FRONTIER_CORE_GPU_LOAD_CONTEXT_H

namespace devils_engine { namespace painter { class assets_base; class graphics_base; } }

namespace tile_frontier {
namespace core {

using namespace devils_engine;

// Контекст GPU-стороны: поток рендера передаёт его в load_warm/unload_hot ресурса через handle
// (warm↔hot исполняет рендер, а не поток ассетов — см. resource_loader).
struct gpu_load_context {
  painter::assets_base* assets;
  painter::graphics_base* base;
};

}
}

#endif
