#ifndef DEVILS_ENGINE_GN06_SITE_MOTIF_RESOURCE_H
#define DEVILS_ENGINE_GN06_SITE_MOTIF_RESOURCE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/originator/motif_geometry.h"

// Авторский ресурс GN06, не язык тематики ядра originator. Владение строками/рецептами
// у каталога; найденная ссылка действительна только пока каталог живёт и не изменяется.

namespace gn06 {

struct part_decl {
  std::string name;
  std::vector<uint32_t> width;
  std::vector<uint32_t> width_max;
  std::vector<uint32_t> height;
  std::vector<uint32_t> height_max;
  std::vector<std::string> sides;
  std::string align;
  std::string passage_align;
  std::string attach_to;
  std::string join;
  std::string shape;
};

struct path_end_decl {
  std::string policy;
  std::string accent;
  uint32_t clearance = 2;
  uint32_t minimum_length = 4;
  uint32_t trim_percent = 75;
};

struct path_decl {
  std::string mode;
  std::vector<uint32_t> l1_per_mille;
  std::vector<uint32_t> l2_per_mille;
  std::vector<int32_t> bend;
  std::vector<uint32_t> l1_max;
  std::vector<uint32_t> l2_max;
  std::vector<int32_t> bend_max;
  uint32_t mirror = 0;
  uint32_t min_bend_length = 1;
  path_end_decl start;
  path_end_decl end;
};

// Рецепт в локальных единицах, а не в клетках изображения.
struct site_motif_geometry {
  std::string operation;
  std::vector<devils_engine::originator::motif_geometry_parameter> parameters;
  std::vector<uint32_t> width;
  std::vector<uint32_t> width_max;
  std::vector<uint32_t> height;
  std::vector<uint32_t> height_max;
  std::string height_policy;
  uint32_t frontage_margin = 0;
  uint32_t frontage_gap = 0;
  std::vector<part_decl> parts;
  path_decl path;
};

struct site_motif_placement {
  std::string strategy;
  uint32_t entrance_edge = 3;
  // Индексы рёбер основного контура: north/east/south/west в локальном кадре.
  std::vector<uint32_t> wall_edges;
};

struct site_motif_presentation {
  std::string glyph;
  std::vector<uint32_t> colour;
};

struct site_motif_resource {
  std::string name;
  site_motif_geometry geometry;
  site_motif_placement placement;
  site_motif_presentation presentation;
};

struct site_motif_catalogue {
  uint32_t version = 0;
  std::vector<std::string> profiles;
  std::vector<site_motif_resource> motifs;
};

// Чтение ресурса не требует темы, графа, legacy-программы motifs или просмотрщика.
site_motif_catalogue read_site_motifs(std::string_view source);
// Точка расширения геометрических процедур этой площадки. Реестр неизменяем при генерации.
const devils_engine::originator::motif_geometry_registry& site_geometry_operations();
const site_motif_resource& find_site_motif(const site_motif_catalogue& catalogue,
                                          std::string_view name);

} // namespace gn06

#endif
