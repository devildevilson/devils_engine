#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1, rg32f) uniform writeonly image2D hiz_image;

// Нулевой уровень пирамиды глубины: копия буфера глубины в пару (min, max). Отдельный шейдер, а не вариант
// понижения по specialization-константе, потому что читает он ДРУГОЙ ресурс — а Vulkan требует, чтобы в layout
// присутствовали все объявленные в шейдере биндинги, даже отключённые константой (урок среза 6). Держать
// биндинг глубины во всех шести дескрипторах цепочки было бы хуже, чем иметь два шейдера.
//
// Уровень 0 хранит одно и то же значение в обеих компонентах: интервал из одного отсчёта. Смысл появляется
// выше, где интервал начинает покрывать блок пикселей.
void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(hiz_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  // texelFetch, а не texture: пирамида обязана быть КОНСЕРВАТИВНОЙ, а интерполяция сглаживает и минимум, и
  // максимум внутрь интервала — то есть ровно ломает то свойство, ради которого пирамида и строится.
  const float depth = texelFetch(depth_image, pixel, 0).r;
  imageStore(hiz_image, pixel, vec4(depth, depth, 0.0, 0.0));
}
