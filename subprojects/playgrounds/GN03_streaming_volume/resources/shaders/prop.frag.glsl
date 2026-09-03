#version 450

#include "camera_block.glsl"

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_position;
layout(location = 2) flat in uint in_kind;

layout(location = 0) out vec4 out_colour;

// Цвет по РОДУ, и род пришёл из поля: столб на открытой площадке, валун на склоне, кристалл под
// сводом. Это и есть смысл сущностей на этой площадке — они не расставлены по миру, а выведены из
// него, и по цвету видно, что генератор про место знает.
void main() {
  const vec3 view_direction = normalize(camera.camera_position.xyz - in_position);
  vec3 normal = normalize(in_normal);
  if (dot(normal, view_direction) < 0.0) {
    normal = -normal;
  }

  // Род в младшем байте, ПОМЕТКА выше. Пометка приходит не от генератора, а из ПАМЯТИ мира: мир таким
  // не рождался, его таким запомнили.
  const uint kind = in_kind & 0xffu;
  const bool marked = ((in_kind >> 8u) & 1u) != 0u;

  vec3 albedo = vec3(0.62, 0.55, 0.38); // столб
  float glow = 0.0;
  if (kind == 1u) {
    albedo = vec3(0.44, 0.43, 0.45); // валун
  } else if (kind == 2u) {
    albedo = vec3(0.22, 0.72, 0.70); // кристалл
    // Собственное свечение, потому что кристалл стоит под сводом, куда солнце не приходит вовсе: без
    // него сущность в пещере не отличается от черноты, а ради неё пещеру и стоит осматривать.
    glow = 0.35;
  }

  if (marked) {
    // Помеченная веха видна издалека и НЕ похожа ни на один род: пометка это не свойство мира, а
    // память о нём, и путать её с родом нельзя.
    albedo = vec3(1.00, 0.78, 0.20);
    glow = 0.5;
  }

  const float sun = max(dot(normal, camera.sun_direction.xyz), 0.0);
  const float sky = 0.5 + 0.5 * normal.y;
  const vec3 light = vec3(1.0, 0.96, 0.88) * sun + camera.sky_colour.rgb * sky * 0.45;
  vec3 colour = albedo * light + albedo * glow;

  const float distance_to_camera = length(camera.camera_position.xyz - in_position);
  const float fog = clamp(pow(distance_to_camera / max(camera.params.y, 1.0), 2.2), 0.0, 1.0);
  colour = mix(colour, camera.sky_colour.rgb, fog);

  out_colour = vec4(colour, 1.0);
}
