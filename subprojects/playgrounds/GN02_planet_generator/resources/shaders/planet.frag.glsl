#version 450

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;
  vec4 viewport_near;
} camera;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_colour;
layout(location = 2) flat in vec3 in_flat_colour;
layout(location = 3) in float in_height;
layout(location = 4) in float in_edge;
layout(location = 5) in float in_selected;

layout(location = 0) out vec4 out_colour;

void main() {
  // Категориальные поля (плиты, провинции, культуры) рисуются ПЛОСКО: интерполяция между двумя
  // случайными цветами меток даёт третий цвет, которого нет ни у одной области, и карта врёт.
  // Непрерывные поля наоборот интерполируются, иначе рельеф и температура становятся ступенчатыми.
  const vec3 base = camera.params.y > 0.5 ? in_flat_colour : in_colour;

  const vec3 normal = normalize(in_normal);
  const float diffuse = max(dot(normal, -camera.light_direction.xyz), 0.0);
  // Терминатор смягчён: у глобуса он занимает половину видимого диска, и жёсткая граница читается
  // как дефект отрисовки, а не как ночь.
  const float shading = 0.42 + 0.70 * smoothstep(0.0, 0.35, diffuse) * (0.35 + 0.65 * diffuse);

  const vec3 view_direction = normalize(camera.camera_position.xyz);
  const float rim = pow(1.0 - max(dot(normal, view_direction), 0.0), 3.0);
  vec3 colour = base * shading + vec3(0.10, 0.16, 0.26) * rim * 0.55;

  // ГРАНИЦА ОБЛАСТИ РИСУЕТСЯ ЛИНИЕЙ, а не стыком двух цветов, и только у категориальных полей.
  //
  // Иначе граница остаётся ломаной решётки: цвет метки смешивать нельзя (смесь двух случайных цветов
  // даёт третий, которого нет ни у одной области), поэтому переход обязан быть резким — а резкий
  // переход по вершинам сетки и есть та самая полигональная ломаная, из-за которой видно решётку
  // Фибоначчи, а не мир.
  //
  // `in_edge` — сглаженная по графу близость к границе области: у центра области нуль, на границе
  // близко к единице. Порог берётся посередине, а полуширина — из экранной производной, поэтому линия
  // имеет одну толщину и вблизи, и у горизонта, и не рассыпается на пиксели при повороте.
  if (camera.params.y > 0.5) {
    const float half_width = max(fwidth(in_edge), 1e-5) * 1.2;
    const float line = smoothstep(0.5 - half_width, 0.5 + half_width, in_edge);
    colour = mix(colour, colour * 0.30, line);
  }

  // ВЫДЕЛЕНИЕ. Не заливка чужим цветом, а подсветка своего: цвет области — это её имя на карте, и
  // подменять его на «цвет выделения» значит терять то, что выделили. Край подсветки берётся из
  // экранной производной, как и граница.
  if (in_selected > 0.0) {
    const float half_width = max(fwidth(in_selected), 1e-5) * 1.2;
    const float inside = smoothstep(0.5 - half_width, 0.5 + half_width, in_selected);
    colour = mix(colour, min(colour * 1.55 + vec3(0.14, 0.14, 0.10), vec3(1.0)), inside);
  }

  out_colour = vec4(colour, 1.0);
}
