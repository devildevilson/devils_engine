#version 450

// Вершина поверхности приходит НЕ через вершинный буфер, а из storage-буфера по gl_VertexIndex.
// Причина простая: сетка процедурная, у неё нет ни атрибутов, ни индексов, а треугольники уже
// развёрнуты по три вершины на CPU — общие вершины не нужны, потому что нормаль это направление.
//
// ВЕРШИНА РЕШАЕТ ФОРМУ, ФРАГМЕНТ РЕШАЕТ ЦВЕТ, и это не стиль, а необходимость. Смещение поверхности
// обязано быть на вершине: двигать геометрию больше негде. А выбор области на вершине НЕВОЗМОЖЕН:
// метка области дискретна, и любая интерполяция между вершинами смешивает две разные метки. Пока
// цвет считался здесь, карта показывала треугольники сетки, а не мир.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;        // масштаб рельефа, режим областей (0 нет, 1 есть, 2 есть с линией), радиус, берег
  vec4 viewport_near;
} camera;

layout(set = 0, binding = 1, std430) readonly buffer SurfaceVertices { vec4 vertices[]; } surface;
layout(set = 0, binding = 2, std430) readonly buffer CellVisuals { vec4 cells[]; } visuals;

layout(location = 0) out vec3 out_normal;
// Направление точки поверхности В СИСТЕМЕ ПЛАНЕТЫ. Это и есть то, по чему фрагмент сам находит свою
// клетку: у решётки Фибоначчи нет полигонов, поэтому «какая клетка здесь» — вопрос о ближайшей точке,
// и задать его можно в любом месте, а не только на вершине.
layout(location = 1) out vec3 out_direction;
layout(location = 2) out float out_height;
// Клетка, ближайшая к ВЕРШИНЕ, плоским значением. Фрагменту она нужна не как ответ, а как НАЧАЛО
// поиска: треугольник мельче клетки, поэтому настоящая ближайшая клетка фрагмента — либо эта, либо
// её сосед по графу.
layout(location = 3) flat out uint out_start_cell;

void main() {
  // Вершина — восемь слов: направление, упакованные веса и четыре номера клеток.
  const vec4 head = surface.vertices[gl_VertexIndex * 2];
  const vec4 tail = surface.vertices[gl_VertexIndex * 2 + 1];
  const vec3 direction = normalize(head.xyz);
  const uint packed = floatBitsToUint(head.w);

  // Веса четырёх ближайших клеток нужны здесь ровно для одного — для ВЫСОТЫ. Высота непрерывна,
  // поэтому её и надо смешивать: с одной ближайшей клеткой у рельефа выходят террасы по клеткам, то
  // есть видна решётка Фибоначчи, которой в мире нет.
  const vec4 raw = vec4(float(packed & 0xffu), float((packed >> 8) & 0xffu),
                        float((packed >> 16) & 0xffu), float((packed >> 24) & 0xffu)) / 255.0;
  const uvec4 cells = uvec4(floatBitsToUint(tail.x), floatBitsToUint(tail.y),
                            floatBitsToUint(tail.z), floatBitsToUint(tail.w));
  const float height = raw.x * visuals.cells[cells.x * 2u].w + raw.y * visuals.cells[cells.y * 2u].w +
                       raw.z * visuals.cells[cells.z * 2u].w + raw.w * visuals.cells[cells.w * 2u].w;

  const float radius = camera.params.z + camera.params.x * height;
  gl_Position = camera.view_projection * camera.planet_to_world * vec4(direction * radius, 1.0);

  out_normal = normalize((camera.planet_to_world * vec4(direction, 0.0)).xyz);
  out_direction = direction;
  out_height = height;
  // Первая клетка списка — ближайшая: на CPU они отсортированы по расстоянию.
  out_start_cell = cells.x;
}
