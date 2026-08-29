// Чтение грубой precipitation-memory в world XZ. Set/binding задаёт consumer перед include:
// sky.frag использует set 0/binding 6, scene.frag — set 2/binding 4.

#ifndef PF08_SURFACE_MEMORY_GLSL
#define PF08_SURFACE_MEMORY_GLSL

#ifndef PF08_SURFACE_MEMORY_SET
#error PF08_SURFACE_MEMORY_SET must be defined before including pf08_surface_memory.glsl
#endif
#ifndef PF08_SURFACE_MEMORY_BINDING
#error PF08_SURFACE_MEMORY_BINDING must be defined before including pf08_surface_memory.glsl
#endif

layout(set = PF08_SURFACE_MEMORY_SET, binding = PF08_SURFACE_MEMORY_BINDING, std430)
readonly buffer SurfacePrecipitationMemory { vec4 cells[]; } pf08_surface_memory_data;

const int pf08_surface_memory_side = 64;

vec4 pf08_surface_memory_fetch(const ivec2 cell) {
  const ivec2 bounded = clamp(cell, ivec2(0), ivec2(pf08_surface_memory_side - 1));
  return pf08_surface_memory_data.cells[bounded.y * pf08_surface_memory_side + bounded.x];
}

vec4 pf08_sample_surface_memory(const pf08_sky_block sky, const vec2 world_xz) {
  const float half_extent = max(sky.surface_weather_shape.x, 1.0);
  const float cell_size = max(sky.surface_weather_shape.y, 0.25);
  const vec2 grid = (world_xz + half_extent) / cell_size - 0.5;
  if (any(lessThan(grid, vec2(-0.5))) ||
      any(greaterThan(grid, vec2(float(pf08_surface_memory_side) - 0.5)))) return vec4(0.0);
  const ivec2 base = ivec2(floor(grid));
  const vec2 fraction = fract(grid);
  const vec4 a = mix(pf08_surface_memory_fetch(base),
                     pf08_surface_memory_fetch(base + ivec2(1, 0)), fraction.x);
  const vec4 b = mix(pf08_surface_memory_fetch(base + ivec2(0, 1)),
                     pf08_surface_memory_fetch(base + ivec2(1, 1)), fraction.x);
  return mix(a, b, fraction.y);
}

#endif
