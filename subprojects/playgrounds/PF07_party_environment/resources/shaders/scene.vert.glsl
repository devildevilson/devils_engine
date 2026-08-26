#version 450

#include "pf07_records.glsl"
#include "pf07_wind.glsl"

// Геометрия сцены: коробки, участок долины, диск земли и заросли — один меш на пару, всё остальное
// делает инстанс. Масштаб оси-выровненный, поэтому нормаль грани остаётся точной без обратной
// транспонированной матрицы. Освещение целиком попиксельное.
//
// Признак материала в `position_material.w` разделяет два поведения: жёсткий предмет и растение.
// Растение получает свой поворот вокруг вертикали и качание от общего ветрового поля. Ветка здесь
// дешевле отдельного материала: отдельный материал потянул бы за собой второй шаг, а шаг проходит по
// ВСЕМ парам группы, то есть рисовал бы заодно и коробки.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_position_material;
layout(location = 3) in vec4 in_half_roughness;
layout(location = 4) in vec4 in_albedo_yaw;

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 2, binding = 0, std140) uniform SkyBlock {
  pf07_sky_block sky;
} sky_data;

layout(location = 0) out vec3 out_world_position;
layout(location = 1) out vec3 out_world_normal;
layout(location = 2) out vec3 out_albedo;

void main() {
  vec3 local = in_position * in_half_roughness.xyz * 2.0;
  vec3 normal = in_normal;

  if (in_position_material.w > 0.5) {
    // Поворот вокруг вертикали: без него все кусты смотрят в одну сторону и поле читается штампом.
    const float yaw = in_albedo_yaw.w;
    const float cosine = cos(yaw);
    const float sine = sin(yaw);
    local = vec3(local.x * cosine - local.z * sine, local.y, local.x * sine + local.z * cosine);
    normal = vec3(normal.x * cosine - normal.z * sine, normal.y, normal.x * sine + normal.z * cosine);
  }

  vec3 world = local + in_position_material.xyz;
  if (in_position_material.w > 0.5) {
    // Доля высоты берётся из ЛОКАЛЬНОЙ вершины, потому что меш куста построен в единичной коробке:
    // ноль у корня, единица у верхушки. Иначе пришлось бы делить на масштаб прямо здесь.
    world += pf07_wind_sway(world, in_position.y, in_half_roughness.w, sky_data.sky.wind_params);
  }

  out_world_position = world;
  out_world_normal = normal;
  out_albedo = in_albedo_yaw.rgb;
  gl_Position = camera_data.view_projection * vec4(world, 1.0);
}
