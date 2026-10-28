#pragma once

#include <string>
#include <vector>

#include "adventure.h"

namespace gn09 {

// Треугольники в координатах своего чанка. Пары соседних чанков получают одни и те же
// граничные сэмплы поля; фиксированное разбиение куба на тетраэдры не меняется от ключа.
struct surface_vertex {
  point position;
  point normal;
  uint8_t shade = 56;
};
using surface_mesh = std::vector<surface_vertex>;

surface_mesh generate_surface(const adventure& world, const chunk& volume);
struct field_region;
// Та же поверхность по полю, уже подготовленному prepare_chunk_field для этого чанка.
surface_mesh generate_surface(const field_region& region, const chunk& volume);
void append_outpost_surface(const adventure& world, const chunk& volume, surface_mesh& mesh);

struct surface_seam_result {
  bool ok = false;
  std::string cause;
  size_t triangles = 0;
  size_t shared_vertices = 0;
};
surface_seam_result verify_surface_seam(const adventure& world);
surface_seam_result verify_outpost_surface_seam(const adventure& world);

} // namespace gn09
