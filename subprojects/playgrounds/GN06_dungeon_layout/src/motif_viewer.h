#ifndef DEVILS_ENGINE_GN06_MOTIF_VIEWER_H
#define DEVILS_ENGINE_GN06_MOTIF_VIEWER_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gn06 {

struct site_view_scene;

// Проверяет все страницы свойств реального плана тем же formatter, что использует окно,
// включая выбранные и наведённые части. Не создаёт окно и не требует GPU.
void verify_site_viewer_details(const site_view_scene& scene);

// Окно только ПОКАЗЫВАЕТ готовый 2D-план. N/B запрашивают ту же сборку с другим seed;
// R/r и жизненный цикл ресурсов к этому просмотру не относятся.
int run_motif_viewer(uint64_t seed, std::string_view scale, size_t requested_side,
                     int32_t entry_y, bool surface_cut, std::string_view catalogue_text,
                     uint32_t frame_limit);

int run_site_viewer(uint64_t seed, std::string_view scale, int32_t entry_y,
                    bool surface_cut, std::string_view source,
                    std::string_view motifs_source, uint32_t frame_limit);

} // namespace gn06

#endif
