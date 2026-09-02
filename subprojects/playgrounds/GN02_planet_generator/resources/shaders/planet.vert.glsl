#version 450

// Вершина поверхности приходит НЕ через вершинный буфер, а из storage-буфера по gl_VertexIndex.
// Причина простая: сетка процедурная, у неё нет ни атрибутов, ни индексов, а треугольники уже
// развёрнуты по три вершины на CPU — общие вершины не нужны, потому что нормаль это направление.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;        // масштаб рельефа, плоская закраска, радиус, режим
  vec4 viewport_near;
} camera;

layout(set = 0, binding = 1, std430) readonly buffer SurfaceVertices { vec4 vertices[]; } surface;
layout(set = 0, binding = 2, std430) readonly buffer CellVisuals { vec4 cells[]; } visuals;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_colour;
layout(location = 2) flat out vec3 out_flat_colour;
layout(location = 3) out float out_height;
// Насколько уверенно эта точка принадлежит своей клетке: разность двух наибольших весов. У центра
// клетки она близка к единице, на границе двух клеток — к нулю. Это и есть РАССТОЯНИЕ ДО ГРАНИЦЫ
// области, посчитанное не по решётке, а по весам, поэтому нулевая линия у него гладкая, а не
// полигональная.
layout(location = 4) out float out_edge;
// Насколько эта точка принадлежит ВЫДЕЛЕННОЙ области. Считается тем же смешиванием, что и граница,
// поэтому край выделения такой же гладкий, как и граница — иначе выделение показывало бы решётку там,
// где её только что перестали показывать границы.
layout(location = 5) out float out_selected;

void main() {
  // Вершина — восемь слов: направление, упакованные веса и четыре номера клеток.
  const vec4 head = surface.vertices[gl_VertexIndex * 2];
  const vec4 tail = surface.vertices[gl_VertexIndex * 2 + 1];
  const vec3 direction = normalize(head.xyz);
  const uint packed = floatBitsToUint(head.w);

  // ЧЕТЫРЕ КЛЕТКИ, А НЕ ОДНА, и это ответ на «видно решётку, а не мир». С одной ближайшей клеткой
  // вершина наследует её значение целиком: у рельефа выходят террасы по клеткам, у границы области —
  // полигональная ломаная. И то и другое показывает решётку Фибоначчи, которой в мире нет.
  //
  // Показатель степени приходит режимом (`camera.viewport_near.w`). Единица — полное сглаживание
  // непрерывного поля; большое значение — почти ближайшая клетка с мягким краем, и это единственный
  // способ сгладить КАТЕГОРИАЛЬНОЕ поле: смешивать цвета метк нельзя, смесь двух случайных цветов
  // даёт третий, которого нет ни у одной области.
  const float sharpness = max(camera.viewport_near.w, 1.0);
  const vec4 raw = vec4(float(packed & 0xffu), float((packed >> 8) & 0xffu),
                        float((packed >> 16) & 0xffu), float((packed >> 24) & 0xffu)) / 255.0;
  // Разность двух наибольших весов берётся до возведения в степень: степень нужна цвету, а граница
  // обязана лежать там, где две клетки РАВНОПРАВНЫ, и это свойство самих расстояний.
  vec4 weights = pow(raw, vec4(sharpness));
  weights /= max(weights.x + weights.y + weights.z + weights.w, 1e-8);

  const uvec4 cells = uvec4(floatBitsToUint(tail.x), floatBitsToUint(tail.y),
                            floatBitsToUint(tail.z), floatBitsToUint(tail.w));
  // Запись клетки — два слова: цвет с высотой и номер области. Номер нужен границе, и он лежит
  // данными, а не выводится из цвета: равенство цветов — догадка о равенстве метк, а не оно само.
  const vec4 a = visuals.cells[cells.x * 2u];
  const vec4 b = visuals.cells[cells.y * 2u];
  const vec4 c = visuals.cells[cells.z * 2u];
  const vec4 d = visuals.cells[cells.w * 2u];
  const vec4 visual = a * weights.x + b * weights.y + c * weights.z + d * weights.w;

  // ГРАНИЦА ОБЛАСТИ приходит ГОТОВЫМ ПОЛЕМ, размытым по графу соседства, и смешивается СЫРЫМИ весами,
  // а не возведёнными в степень: линия обязана быть гладкой независимо от того, насколько резко
  // смешивается цвет.
  //
  // Первые две попытки были неверны. Разность весов двух ближайших клеток обводит каждую клетку
  // решётки; та же разность, ограниченная разными областями, обводит уже границу области — но по
  // ЛОМАНОЙ решётки, потому что расстояние до серединной поверхности между двумя клетками и есть эта
  // ломаная. Сгладить надо саму ФОРМУ границы, а это делается только усреднением поля по графу.
  const vec4 borders = vec4(visuals.cells[cells.x * 2u + 1u].y, visuals.cells[cells.y * 2u + 1u].y,
                            visuals.cells[cells.z * 2u + 1u].y, visuals.cells[cells.w * 2u + 1u].y);
  out_edge = dot(borders, raw) / max(raw.x + raw.y + raw.z + raw.w, 1e-8);

  const vec4 areas = vec4(visuals.cells[cells.x * 2u + 1u].x, visuals.cells[cells.y * 2u + 1u].x,
                          visuals.cells[cells.z * 2u + 1u].x, visuals.cells[cells.w * 2u + 1u].x);
  const float selection = camera.light_direction.w;
  const vec4 belongs = vec4(areas.x == selection ? 1.0 : 0.0, areas.y == selection ? 1.0 : 0.0,
                            areas.z == selection ? 1.0 : 0.0, areas.w == selection ? 1.0 : 0.0);
  out_selected = selection < 0.5 ? 0.0 : dot(belongs, raw) / max(raw.x + raw.y + raw.z + raw.w, 1e-8);

  const float radius = camera.params.z + camera.params.x * visual.w;
  const vec3 local = direction * radius;
  gl_Position = camera.view_projection * camera.planet_to_world * vec4(local, 1.0);

  out_normal = normalize((camera.planet_to_world * vec4(direction, 0.0)).xyz);
  out_colour = visual.rgb;
  // Плоский цвет берётся у САМОЙ БЛИЖНЕЙ клетки, а не у смеси: он нужен там, где смешивать нельзя
  // совсем, и смесь в нём была бы противоречием.
  out_flat_colour = a.rgb;
  out_height = visual.w;
}
