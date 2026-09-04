#version 450

// СУЩНОСТЬ — ЭКЗЕМПЛЯР, а не сетка в буфере. У вехи двенадцать вершин (четыре боковые грани
// четырёхгранника), и строятся они из gl_VertexIndex: заводить меш ради двенадцати вершин значило бы
// тащить в площадку загрузку моделей, которой её вопрос не касается.
//
// Отличие от вершин поверхности здесь одно, и оно про количество, а не про принцип. Вершин миллионы,
// поэтому они упакованы и лежат в координатах своего чанка, а смещение берут из таблицы. Сущностей
// десяток на чанк, поэтому процессор приводит их к системе чанка камеры сам, каждый кадр: это
// дешевле, чем заводить им вторую арену со слотами.
//
// ОРИЕНТАЦИЯ ПО НОРМАЛИ ПОЛА: веха наклоняется вместе с землёй, а не торчит вертикально из склона.
//
// Поворот вокруг своей оси ПРИЕЗЖАЕТ ДАННЫМИ, восемью битами в слове рода, и это исправление. Он
// брался хешем от позиции — а позиция здесь задана ОТНОСИТЕЛЬНО ЧАНКА КАМЕРЫ и меняется на размер
// чанка, стоит наблюдателю перейти в соседний. Вехи от этого проворачивались на месте: вид предмета
// зависел от того, откуда на него смотрят. Теперь поворот выводится из ИМЕНИ вехи (ключ чанка плюс
// номер попытки) на процессоре — из того же имени, по которому мир её помнит.

#include "camera_block.glsl"

layout(set = 0, binding = 1, std430) readonly buffer Props { vec4 data[]; } props;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_position;
layout(location = 2) flat out uint out_kind;

void main() {
  const vec4 head = props.data[gl_InstanceIndex * 2];
  const vec4 tail = props.data[gl_InstanceIndex * 2 + 1];

  const vec3 base_point = head.xyz;
  const float scale = head.w;
  const vec3 up = normalize(tail.xyz);
  out_kind = floatBitsToUint(tail.w);

  // Базис на поверхности. Опорная ось выбирается по МЕНЬШЕЙ компоненте нормали: с постоянной осью
  // векторное произведение вырождается там, где нормаль с ней совпадает, и веха вырождается в линию.
  const vec3 reference = abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  vec3 tangent = normalize(cross(reference, up));
  const vec3 bitangent = cross(up, tangent);

  // Восемь бит на полный оборот — шаг 1.4 градуса, чего для «стоит не строго на север» хватает.
  const float spin = float((out_kind >> 16u) & 0xffu) * (6.2831853 / 256.0);
  const vec3 axis_a = tangent * cos(spin) + bitangent * sin(spin);
  const vec3 axis_b = bitangent * cos(spin) - tangent * sin(spin);

  // Четыре угла основания и вершина. Основание чуть утоплено, иначе на неровном полу веха стоит на
  // одной точке и выглядит парящей.
  const uint face = uint(gl_VertexIndex) / 3u;
  const uint corner = uint(gl_VertexIndex) % 3u;

  const float width = 0.3 * scale;
  const vec3 sunk = base_point - up * (0.08 * scale);

  vec3 local;
  if (corner == 0u) {
    local = up * scale; // острие
  } else {
    const uint index = face + (corner == 1u ? 0u : 1u);
    const float angle = float(index) * 1.5707963;
    local = axis_a * (width * cos(angle)) + axis_b * (width * sin(angle));
  }

  out_position = sunk + local;
  // Нормаль грани здесь не считается: у вехи она нужна только для затенения, а затенение по нормали
  // ПОЛА даёт ровно то, что нужно глазу — предмет, освещённый как земля под ним.
  out_normal = normalize(mix(up, normalize(local + up * 0.35 * scale), 0.65));
  gl_Position = camera.view_projection * vec4(out_position, 1.0);
}
