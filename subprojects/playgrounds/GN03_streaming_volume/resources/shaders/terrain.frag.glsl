#version 450

#include "camera_block.glsl"

layout(location = 0) in vec3 in_normal;
// Позиция ОТНОСИТЕЛЬНО ЧАНКА КАМЕРЫ, а не мировая: мировых координат в кадре нет вовсе. Для всего,
// что здесь считается — направление на камеру, расстояние для тумана, сетка чанков, — этого хватает,
// потому что начало отсчёта отличается от мирового на целое число размеров чанка.
layout(location = 1) in vec3 in_position;
layout(location = 2) in float in_shade;

layout(location = 0) out vec4 out_colour;

// Затенение здесь служебное, и у каждого решения есть причина, связанная с тем, что площадка
// проверяет.
//
// ДВУСТОРОННЕЕ ОСВЕЩЕНИЕ, потому что отсечение задней грани выключено, а выключено оно потому, что
// камера залетает В ПЕЩЕРУ. Внутри хода видна именно задняя сторона поверхности, и с отсечением
// стена пропала бы — то есть выглядела бы точно как дырка в мире, которую эта площадка и ищет.
//
// ЦВЕТ ПО УКЛОНУ, а не по высоте: у объёма высота ничего не говорит (нависание и пол под ним лежат
// на одной высоте), а уклон отвечает на вопрос «пол это или стена». Заодно уклон — единственное, что
// делает видимой ЛЕСТНИЦУ треугольников, если поверхность посчитана грубо.
void main() {
  const vec3 view_direction = normalize(camera.camera_position.xyz - in_position);
  vec3 normal = normalize(in_normal);
  // Нормаль разворачивается к наблюдателю: снаружи это ничего не меняет, внутри хода спасает стену
  // от полной черноты.
  if (dot(normal, view_direction) < 0.0) {
    normal = -normal;
  }

  const float slope = clamp(1.0 - normal.y, 0.0, 1.0);

  // ЦВЕТ БИОМА ПО ОТТЕНКУ, пришедшему от генератора. Лента, а не таблица: оттенок уже СМЕШАН по весам
  // биомов, поэтому на переходе он лежит между их значениями, и цвет обязан меняться так же плавно.
  // Таблица «индекс биома -> цвет» дала бы резкую границу там, где форма переходит гладко.
  //
  // Точки ленты соответствуют объявленным `biome_*_shade`: плато, степь, карст, бэдленды, горы.
  vec3 biome_colour = vec3(0.30, 0.40, 0.20);                                        // плато
  biome_colour = mix(biome_colour, vec3(0.55, 0.50, 0.24), smoothstep(0.18, 0.32, in_shade)); // степь
  biome_colour = mix(biome_colour, vec3(0.52, 0.52, 0.50), smoothstep(0.32, 0.55, in_shade)); // карст
  biome_colour = mix(biome_colour, vec3(0.52, 0.30, 0.20), smoothstep(0.55, 0.70, in_shade)); // бэдленды
  biome_colour = mix(biome_colour, vec3(0.62, 0.66, 0.72), smoothstep(0.70, 0.86, in_shade)); // горы

  // Уклон поверх биома: пол своего цвета, стена и потолок серее. Уклон отвечает на вопрос «пол это
  // или стена», а биом — «какой это край мира», и это два разных вопроса.
  const vec3 steep_colour = mix(biome_colour, vec3(0.38, 0.35, 0.31), 0.65); // стена
  const vec3 cliff_colour = mix(biome_colour, vec3(0.20, 0.19, 0.18), 0.80); // потолок и обрыв
  vec3 albedo = mix(biome_colour, steep_colour, smoothstep(0.15, 0.65, slope));
  albedo = mix(albedo, cliff_colour, smoothstep(0.80, 1.35, slope));

  const float sun = max(dot(normal, camera.sun_direction.xyz), 0.0);
  // Полусферическое небо вместо постоянной подсветки: без него дно пещеры и северный склон
  // одинаково черны, и рельефа в тени не видно вовсе.
  const float sky = 0.5 + 0.5 * normal.y;
  const vec3 light = vec3(1.0, 0.96, 0.88) * sun + camera.sky_colour.rgb * sky * 0.45;
  vec3 colour = albedo * light;

  // Сетка чанков по требованию. Она и есть отладочное представление этой площадки: шов, если он
  // есть, лежит ровно на этих плоскостях, и без сетки «полоса на склоне» неотличима от рельефа.
  if (camera.params.w > 0.5) {
    // Сетка попадает на границы чанков и в этой системе координат: начало отсчёта отличается от
    // мирового на ЦЕЛОЕ число размеров чанка, поэтому кратные точки те же самые.
    const vec3 chunk_uv = abs(fract(in_position / camera.params.x + 0.5) - 0.5) * camera.params.x;
    const vec3 width = fwidth(in_position) * 1.5 + 1.0e-4;
    const vec3 line = 1.0 - smoothstep(vec3(0.0), width, chunk_uv);
    const float strength = clamp(max(max(line.x, line.y), line.z), 0.0, 1.0);
    colour = mix(colour, vec3(1.0, 0.35, 0.15), strength * 0.75);
  }

  const int mode = int(camera.params.z + 0.5);
  if (mode == 1) {
    colour = normal * 0.5 + 0.5;
  } else if (mode == 2) {
    colour = vec3(slope, 1.0 - slope, 0.2);
  } else if (mode == 3) {
    // Отладочное представление БИОМА: чистый оттенок без света и тумана. Нужно ровно затем, зачем
    // сетка чанков — увидеть границу там, где на затенённой картинке её не отличить от склона.
    out_colour = vec4(vec3(in_shade), 1.0);
    return;
  }

  // Туман до самой границы окна чанков: дальность приходит из окна, а цвет — тот же, что у неба у
  // горизонта. Иначе край мира виден как обрыв в пустоту, и «успевает ли мир строиться» становится
  // вопросом про туман, а не про генератор.
  const float distance_to_camera = length(camera.camera_position.xyz - in_position);
  const float fog = clamp(pow(distance_to_camera / max(camera.params.y, 1.0), 2.2), 0.0, 1.0);
  colour = mix(colour, camera.sky_colour.rgb, fog);

  out_colour = vec4(colour, 1.0);
}
