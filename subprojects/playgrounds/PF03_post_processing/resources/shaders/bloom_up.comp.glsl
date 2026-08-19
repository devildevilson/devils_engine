#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D lower_image;
// Цель читается и пишется: 'general' в конфиге. Так подъём складывается на месте, без ping-pong ресурсов.
layout(set = 1, binding = 1, rgba16f) uniform image2D target_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(target_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec2 texel = 1.0 / vec2(textureSize(lower_image, 0));

  // Шатровый фильтр 3x3 при подъёме. Без него на каждом уровне видны квадраты нижнего разрешения: билинейная
  // интерполяция даёт кусочно-линейную поверхность с изломами на границах текселей.
  vec3 lower = texture(lower_image, uv + texel * vec2(-1.0, -1.0)).rgb * 0.0625;
  lower += texture(lower_image, uv + texel * vec2(0.0, -1.0)).rgb * 0.125;
  lower += texture(lower_image, uv + texel * vec2(1.0, -1.0)).rgb * 0.0625;
  lower += texture(lower_image, uv + texel * vec2(-1.0, 0.0)).rgb * 0.125;
  lower += texture(lower_image, uv).rgb * 0.25;
  lower += texture(lower_image, uv + texel * vec2(1.0, 0.0)).rgb * 0.125;
  lower += texture(lower_image, uv + texel * vec2(-1.0, 1.0)).rgb * 0.0625;
  lower += texture(lower_image, uv + texel * vec2(0.0, 1.0)).rgb * 0.125;
  lower += texture(lower_image, uv + texel * vec2(1.0, 1.0)).rgb * 0.0625;

  const vec3 current = imageLoad(target_image, pixel).rgb;
  imageStore(target_image, pixel, vec4(current + lower * frame.bloom_params.w, 1.0));
}
