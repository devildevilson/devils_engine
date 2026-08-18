#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D scene_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(scene_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const float aspect = float(size.x) / float(size.y);
  const vec2 p = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);

  // Движущийся источник: единственная задача сцены — дать темпоральному пассу что-то, у чего видно
  // накопление. Диск + тонкая орбита, чтобы шлейф читался и по яркости, и по форме.
  const float t = frame.viewport_time.z * frame.controls.y;
  const vec2 center = vec2(cos(t) * 0.28, sin(t * 1.3) * 0.20);
  const float d = length(p - center);
  const float disc = smoothstep(0.055, 0.030, d);

  const vec3 warm = vec3(1.0, 0.55, 0.20);
  const vec3 grid = vec3(0.02, 0.025, 0.035) * (0.5 + 0.5 * step(0.98, max(fract(uv.x * 16.0), fract(uv.y * 9.0))));

  imageStore(scene_image, pixel, vec4(warm * disc * 3.0 + grid, 1.0));
}
