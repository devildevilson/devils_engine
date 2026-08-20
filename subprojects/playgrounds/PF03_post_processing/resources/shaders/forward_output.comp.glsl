#version 450

#include "pf03_shading.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D resolved_image;

layout(set = 2, binding = 0, rgba16f) uniform writeonly image2D composed_image;

// Выход forward-ветви: экспозиция, кривая, дизер. Ровно то, что нужно простому проекту, и ничего больше —
// вся цепочка PF03 здесь не нужна по построению. Небо рисуется тут же, потому что в forward-ветви нет
// G-buffer'а и «пустой пиксель» опознаётся не по глубине, а по альфе резолва.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(composed_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec4 resolved = texture(resolved_image, uv);

  // Альфа резолва меньше единицы означает, что в пикселе была не только геометрия: на кромке она равна доле
  // покрытых сэмплов. Это и есть то, за что MSAA платят — и заодно способ подмешать небо ровно по покрытию.
  const vec3 sky = pf03_sky_color(uv, frame.exposure_limits.w);
  const vec3 scene = mix(sky, resolved.rgb / max(resolved.a, 1.0e-4), clamp(resolved.a, 0.0, 1.0));

  const float exposure = frame.tonemap.y > 0.0 ? frame.tonemap.y : 0.1;
  vec3 result = pf03_apply_tonemap(scene * exposure, int(frame.tonemap.x + 0.5));

  if (frame.output_params.x > 0.5) {
    result += pf03_triangular_dither(vec2(pixel), frame.output_params.y) / 255.0;
  }
  if (frame.controls.w > 0.5) {
    result = pf03_encode_srgb(result);
  }

  imageStore(composed_image, pixel, vec4(result, 1.0));
}
