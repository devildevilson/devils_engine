#version 450

// Алгоритм: directional Lambert сначала даёт непрерывный N.L. В cel mode диапазон 0..1 делится на N уровней:
// соседние уровни соединяет управляемая узкая smoothstep-зона, поэтому bands остаются читаемыми, но не мерцают
// от бесконечно жёсткой границы. Ambient добавляется ПОСЛЕ квантования и не уничтожает тёмную полосу. Material id
// выбирает fixture-цвет; bit 8 слегка подсвечивает выбранный через world UI scene object, не меняя его geometry id.
// Второй MRT сохраняет непрерывную world normal для decals и feature-outline.

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) flat in float material_id;
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 normal_target;

layout(set = 1, binding = 0, std140) uniform CelSettingsBlock {
  // lighting: enabled, band count, transition softness in one-cell units, ambient fraction.
  vec4 lighting;
  // outline: policy (0 off, 1 silhouette/depth, 2 feature/depth+normal), width px, depth threshold, normal cosine.
  vec4 outline;
  vec4 outline_color;
} cel;

float quantized_lighting(const float value, const float band_count, const float softness) {
  const float intervals = max(floor(band_count + 0.5) - 1.0, 1.0);
  const float scaled = clamp(value, 0.0, 1.0) * intervals;
  const float lower_index = min(floor(scaled), intervals - 1.0);
  const float fraction = scaled - lower_index;
  const float transition = smoothstep(
    0.5 - clamp(softness, 0.0, 0.49),
    0.5 + clamp(softness, 0.0, 0.49),
    fraction);
  return (lower_index + transition) / intervals;
}

void main() {
  const bool selected = material_id >= 8.0;
  const float fixture_id = selected ? material_id - 8.0 : material_id;
  vec3 base = vec3(0.17, 0.20, 0.24);
  if (fixture_id > 0.5) base = vec3(0.28, 0.20, 0.14);
  if (fixture_id > 1.5) base = vec3(0.12, 0.25, 0.22);
  if (fixture_id > 2.5) base = vec3(1.00, 0.28, 0.035);
  if (fixture_id > 3.5) base = vec3(0.20, 0.42, 0.82);
  if (selected) base = mix(base, vec3(0.18, 0.72, 0.96), 0.32);
  const vec3 light_dir = normalize(vec3(-0.4, 0.8, 0.5));
  float lambert = max(dot(normalize(world_normal), light_dir), 0.0);
  if (cel.lighting.x > 0.5) {
    lambert = quantized_lighting(lambert, cel.lighting.y, cel.lighting.z);
  }
  const float ambient = clamp(cel.lighting.w, 0.0, 1.0);
  const float diffuse = ambient + (1.0 - ambient) * lambert;
  frag_color = vec4(base * diffuse, 1.0);
  normal_target = vec4(normalize(world_normal) * 0.5 + 0.5, 1.0);
}
