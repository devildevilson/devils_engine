#version 450

// Вершины вытягиваются из storage-буфера, а не подаются вершинным вводом: поток геометрии меняется
// вместе с резидентностью секторов, и переписывать один отображённый буфер дешевле, чем пересобирать
// device-local вершинный буфер на каждый шаг камеры.
struct zone_vertex {
  vec3 position;   // мировые метры: XZ — план, Y — высота стены или пола
  uint tint;       // упакованный RGBA
  uint zone_slot;  // индекс зоны в текущем кадре; 0xffffffff — «ничья» геометрия вроде персонажа
  uint pad0;
  uint pad1;
  uint pad2;
};

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 view;
  vec4 camera_position;
  vec4 viewport_near;
} camera_data;

layout(set = 0, binding = 1, std430) readonly buffer VertexStream {
  zone_vertex vertices[];
} stream;

layout(location = 0) out vec4 out_color;

vec4 unpack_tint(const uint value) {
  return vec4(float((value >> 0) & 0xffu), float((value >> 8) & 0xffu), float((value >> 16) & 0xffu),
              float((value >> 24) & 0xffu)) / 255.0;
}

void main() {
  const zone_vertex vertex = stream.vertices[gl_VertexIndex];
  gl_Position = camera_data.view_projection * vec4(vertex.position, 1.0);

  vec4 tint = unpack_tint(vertex.tint);

  // Выделение и наведение решаются СРАВНЕНИЕМ, а не перезаписью буфера: подсветка меняется каждый кадр
  // вслед за курсором, и перезаливать из-за неё геометрию значило бы платить за мышь трафиком.
  const uint selected = floatBitsToUint(camera_data.camera_position.z);
  const uint hovered = floatBitsToUint(camera_data.camera_position.w);
  if (vertex.zone_slot != 0xffffffffu) {
    if (vertex.zone_slot == selected) tint = vec4(mix(tint.rgb, vec3(1.0, 0.85, 0.25), 0.65), tint.a);
    else if (vertex.zone_slot == hovered) tint = vec4(mix(tint.rgb, vec3(1.0), 0.35), tint.a);
  }
  out_color = tint;
}
