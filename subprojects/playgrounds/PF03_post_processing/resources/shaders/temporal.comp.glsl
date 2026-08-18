#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D scene_image;
// Прошлый кадр приходит ОТДЕЛЬНЫМ биндингом (в конфиге у него 'history = 1'), а не элементом массива
// копий: номер кадра шейдеру не нужен ни как данные, ни как индекс. Копию гарантированно уже дописал
// предыдущий кадр — порядок движок выводит из этого же объявления.
layout(set = 1, binding = 1) uniform sampler2D history_image;
layout(set = 2, binding = 0, rgba16f) uniform writeonly image2D feedback_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(feedback_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec3 current = texture(scene_image, uv).rgb;
  const vec3 history = texture(history_image, uv).rgb;

  // Вес истории обнуляется на первых кадрах после сброса: истории физически ещё нет (движок чистит копии
  // в нули), и без этого первые кадры выглядели бы как затемнение, а не как отсутствие шлейфа.
  const float warmup = step(1.5, frame.viewport_time.w);
  const float weight = frame.controls.x * warmup;

  imageStore(feedback_image, pixel, vec4(max(current, history * weight), 1.0));
}
