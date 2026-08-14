#version 450

#ifndef PF01_TILE_SIZE
#define PF01_TILE_SIZE 16
#endif
#ifndef PF01_MAX_LIGHTS_PER_TILE
#define PF01_MAX_LIGHTS_PER_TILE 64
#endif
#ifndef PF01_TILES_X
#define PF01_TILES_X 120
#endif
#ifndef PF01_TILES_Y
#define PF01_TILES_Y 68
#endif
#ifndef PF01_SPECULAR_POWER
#define PF01_SPECULAR_POWER 32.0
#endif

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data[3];

layout(set = 0, binding = 1, std430) readonly buffer LightBuffer {
  vec4 words[];
} light_data[3];

layout(set = 1, binding = 0, std430) readonly buffer TileBuffer {
  uint words[];
} tile_data[3];

vec3 wall_albedo(vec3 normal, vec2 texcoord) {
  vec3 albedo = vec3(0.72);
  if (normal.y > 0.8) albedo = vec3(0.82);
  if (normal.y < -0.8) albedo = vec3(0.48);
  if (normal.x > 0.8) albedo = vec3(0.68, 0.72, 0.78);
  if (normal.x < -0.8) albedo = vec3(0.78, 0.70, 0.66);
#ifdef PF01_CHECKER_WALL
  if (normal.z > 0.8) {
    const ivec2 cell = ivec2(floor(texcoord * 12.0));
    const float checker = ((cell.x + cell.y) & 1) == 0 ? 0.32 : 0.9;
    albedo = vec3(checker);
  }
#endif
  return albedo;
}

void main() {
  const vec3 normal = normalize(world_normal);
  const vec3 albedo = wall_albedo(normal, uv);
  const vec3 view_direction = normalize(camera_data[0].camera_position.xyz - world_position);
  const uvec2 tile = uvec2(gl_FragCoord.xy) / PF01_TILE_SIZE;
  if (tile.x >= PF01_TILES_X || tile.y >= PF01_TILES_Y) {
    out_color = vec4(albedo * 0.025, 1.0);
    return;
  }
  const uint list_base = (tile.y * PF01_TILES_X + tile.x) * (PF01_MAX_LIGHTS_PER_TILE + 1);
  const uint count = min(tile_data[0].words[list_base], uint(PF01_MAX_LIGHTS_PER_TILE));

  vec3 lighting = albedo * 0.025;
  for (uint item = 0u; item < count; ++item) {
    const uint light_index = tile_data[0].words[list_base + 1u + item];
    const vec4 position_radius = light_data[0].words[1u + light_index * 2u];
    const vec4 color_intensity = light_data[0].words[2u + light_index * 2u];
    const vec3 to_light = position_radius.xyz - world_position;
    const float distance_to_light = length(to_light);
    if (distance_to_light >= position_radius.w) continue;
    const vec3 light_direction = to_light / max(distance_to_light, 0.0001);
    const float edge = max(1.0 - distance_to_light / position_radius.w, 0.0);
    const float attenuation = edge * edge;
    const float diffuse = max(dot(normal, light_direction), 0.0);
    const vec3 half_direction = normalize(light_direction + view_direction);
    const float specular = pow(max(dot(normal, half_direction), 0.0), PF01_SPECULAR_POWER) * step(0.0, diffuse);
    lighting += (albedo * diffuse + vec3(0.22) * specular) * color_intensity.rgb * color_intensity.a * attenuation;
  }

  const vec3 mapped = lighting / (lighting + vec3(1.0));
  out_color = vec4(pow(mapped, vec3(1.0 / 2.2)), 1.0);
}
