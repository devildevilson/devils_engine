#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(constant_id = 0) const int shaft_samples = 24;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D bright_image;
layout(set = 1, binding = 1) uniform sampler2D depth_image;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image2D shafts_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(shafts_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);

  // Солнце вне кадра — лучей нет. Это принципиальное ограничение экранного метода: он умеет тянуть лучи
  // только от источника, который виден, и не знает про затенители за границей кадра. Полноценные шахты
  // требуют марша по карте теней, то есть отдельного среза.
  if (frame.shaft_params.z <= 0.0) {
    imageStore(shafts_image, pixel, vec4(0.0));
    return;
  }

  const vec2 sun_uv = frame.shaft_params.xy;
  const vec2 to_sun = sun_uv - uv;

  // Радиальное размытие: идём от пикселя к солнцу и собираем яркость там, где НЕТ геометрии. Именно поэтому
  // маска умножается на признак неба: лучи это свет, дошедший мимо препятствий, а не свечение поверхностей.
  vec3 accumulated = vec3(0.0);
  float weight_sum = 0.0;
  for (int i = 0; i < shaft_samples; ++i) {
    const float t = float(i) / float(shaft_samples - 1);
    const vec2 sample_uv = uv + to_sun * t;
    if (any(lessThan(sample_uv, vec2(0.0))) || any(greaterThan(sample_uv, vec2(1.0)))) {
      continue;
    }

    const float depth = texture(depth_image, sample_uv).r;
    const float sky = depth <= 0.0 ? 1.0 : 0.0;
    // Затухание вдоль луча: вклад дальних от пикселя проб убывает, иначе лучи выглядят одинаково яркими на
    // всю длину и превращаются в засветку кадра.
    const float falloff = exp(-t * frame.shaft_params.w);
    accumulated += texture(bright_image, sample_uv).rgb * sky * falloff;
    weight_sum += falloff;
  }

  const vec3 result = weight_sum > 0.0 ? accumulated / weight_sum : vec3(0.0);
  imageStore(shafts_image, pixel, vec4(result * frame.shaft_params.z, 1.0));
}
