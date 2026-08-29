#version 450

#include "pf08_precipitation.glsl"

// Очень дешёвая карта ПАМЯТИ МИРА. Она хранит не материал и не готовый цвет, а только то, сколько
// дождевой/снежной воды недавно получил участок. Биом/материал решает, как показать эти два числа.
// 64x64 по 8 м — намеренно грубо: фронт осадков измеряется сотнями метров, сантиметровая карта здесь
// была бы дорогой ложной точностью.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SkyBlock { pf08_sky_block sky; } sky_data;
layout(set = 0, binding = 1, std430) writeonly buffer CurrentSurfaceMemory { vec4 cells[]; } current_memory;
layout(set = 0, binding = 2, std430) readonly buffer PreviousSurfaceMemory { vec4 cells[]; } previous_memory;

const uint pf08_surface_memory_side = 64u;

void main() {
  const uvec2 cell = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(cell, uvec2(pf08_surface_memory_side)))) return;
  const uint index = cell.y * pf08_surface_memory_side + cell.x;
  const pf08_sky_block sky = sky_data.sky;

  const float half_extent = max(sky.surface_weather_shape.x, 1.0);
  const float cell_size = max(sky.surface_weather_shape.y, 0.25);
  const vec2 world_xz = (vec2(cell) + 0.5) * cell_size - half_extent;
  const float footprint = pf08_precipitation_field_density(sky, world_xz);

  // Каждая per-frame copy впервые встречается в первых кадрах. В этот момент history ещё не имеет
  // смысла, и ячейка получает только явно заданную prewarm-память, модулированную мировым фронтом.
  const bool startup = sky.precipitation_time.y < 4.0;
  vec4 memory = startup
    ? vec4(sky.surface_weather.x * footprint, sky.surface_weather.y * footprint, 0.0, 0.0)
    : previous_memory.cells[index];

  const float real_dt = min(max(sky.precipitation_time.x, 0.0), 0.05);
  const float world_hours = real_dt * max(sky.surface_weather_shape.z, 0.0) / 3600.0;
  const float dry_half_life_h = max(sky.surface_weather.z, 1e-3);
  const float snow_melt_mm_h = max(sky.surface_weather_shape.w, 0.0);
  const float rain_rate = max(sky.precipitation_params.x, 0.0) * footprint;
  const float snow_rate = max(sky.snow_params.x, 0.0) * footprint;

  // F из постановки задачи — это буквально интеграл rate*duration. Дождевой reservoir затем
  // экспоненциально высыхает; снег хранится как водный эквивалент и линейно тает только когда новый
  // снег почти не идёт. Дождь ускоряет таяние и переводит талую воду в rain-memory.
  memory.x *= exp2(-world_hours / dry_half_life_h);
  memory.x += rain_rate * world_hours;
  memory.y += snow_rate * world_hours;
  if (snow_rate < 0.01 && memory.y > 0.0) {
    const float melt = min(memory.y, (snow_melt_mm_h + rain_rate * 0.35) * world_hours);
    memory.y -= melt;
    memory.x += melt;
  }

  const float rate_blend = 1.0 - exp(-real_dt * 3.0);
  memory.z = mix(memory.z, rain_rate, rate_blend);
  memory.w = mix(memory.w, snow_rate, rate_blend);
  current_memory.cells[index] = clamp(memory, vec4(0.0), vec4(16.0, 24.0, 100.0, 100.0));
}
