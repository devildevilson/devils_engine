#version 450

// Алгоритм: однократное атмосферное рассеяние в экспоненциальном по высоте тумане. По depth восстанавливается
// мировая точка, а оптическая толщина вдоль луча берётся аналитическим интегралом — без marching. Закон
// Бугера даёт transmittance, Henyey-Greenstein направляет рассеянный солнечный свет, затем поверхность
// смешивается с in-scattering в линейном HDR. Небо исключено как уже содержащее атмосферу; T пишется в alpha.

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0) uniform sampler2D scene_image;

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D fogged_image;

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(fogged_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec3 scene = texture(scene_image, uv).rgb;
  const float depth = texture(depth_image, uv).r;

  // Небо туманом не закрашиваем: атмосфера в него уже «встроена», иначе получится двойной учёт и горизонт
  // уедет в цвет рассеяния дважды.
  if (depth <= 0.0) {
    imageStore(fogged_image, pixel, vec4(scene, 1.0));
    return;
  }

  const vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
  const vec4 world_h = frame.inverse_view_projection * ndc;
  const vec3 world = world_h.xyz / world_h.w;

  const vec3 camera = frame.camera_position.xyz;
  const vec3 to_point = world - camera;
  const float distance_along_ray = length(to_point);
  const vec3 direction = distance_along_ray > 1.0e-5 ? to_point / distance_along_ray : vec3(0.0, 0.0, 1.0);

  const float tau = pf03_fog_optical_depth(
    camera, direction, distance_along_ray,
    frame.fog_params.x, frame.fog_params.y, frame.fog_params.z);

  // Пропускание по закону Бугера: сколько света от поверхности доходит до камеры. Остальная доля кадра —
  // то, что рассеялось В луч по дороге, и именно она съедает контраст на дальних планах (аэроперспектива).
  const float transmittance = exp(-max(tau, 0.0));

  const vec3 sun_direction = normalize(frame.light_direction.xyz);
  const float cos_theta = dot(direction, sun_direction);
  const float phase = pf03_phase_hg(cos_theta, clamp(frame.fog_params.w, -0.9, 0.9));

  // Рассеянный свет: изотропная часть (небо) плюс направленная от солнца. Нормировка 4pi возвращает фазовую
  // функцию к «единица при изотропном рассеянии», чтобы яркость тумана не зависела от выбора анизотропии.
  const vec3 inscatter = frame.fog_color.rgb * (1.0 + frame.fog_color.w * (phase * 4.0 * 3.14159265 - 1.0));

  const vec3 result = scene * transmittance + inscatter * (1.0 - transmittance);

  // Пропускание кладём в альфу: пасс компоновки показывает его как отдельный вид, и это же значение
  // понадобится будущим эффектам (например god rays), чтобы не считать туман заново.
  imageStore(fogged_image, pixel, vec4(result, transmittance));
}
