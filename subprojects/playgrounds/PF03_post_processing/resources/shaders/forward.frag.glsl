#version 450

#include "pf03_shading.glsl"

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(location = 0) in vec3 world_normal;
layout(location = 1) in vec3 world_position;

layout(location = 0) out vec4 out_color;

// Освещение считается ПРЯМО ПРИ РАСТЕРИЗАЦИИ, и это и есть содержание развилки. При MSAA фрагментный шейдер
// выполняется раз на ПИКСЕЛЬ, а результат пишется во все покрытые сэмплы — поэтому многосэмпловое сглаживание
// достаётся геометрическим кромкам бесплатно, а шейдинговый аляйсинг (спекуляр, шахматка на полу) не
// исправляется вовсе. Из этого же следует, почему у MSAA не может быть G-buffer'а: шейдинг из многосэмплового
// G-buffer'а потребовал бы либо per-sample шейдинга (цена x N), либо выбора одного сэмпла — то есть отказа от
// самого сглаживания.
void main() {
  const vec3 normal = normalize(world_normal);
  // AO единица: экранные эффекты читают один сэмпл и с MSAA не согласуются, поэтому в этой ветви их нет
  const vec3 color = pf03_shade_surface(
    world_position, normal, frame.camera_position.xyz, frame.light_direction.xyz,
    frame.exposure_limits.w, frame.light_direction.w, 1.0, frame.output_params.z);
  out_color = vec4(color, 1.0);
}
