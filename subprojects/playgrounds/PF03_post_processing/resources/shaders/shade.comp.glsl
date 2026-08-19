#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0, rgba16f) uniform writeonly image2D scene_image;

vec3 world_position_from_depth(const vec2 uv, const float reverse_depth) {
  const vec4 ndc = vec4(uv * 2.0 - 1.0, reverse_depth, 1.0);
  const vec4 world = frame.inverse_view_projection * ndc;
  return world.xyz / world.w;
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(scene_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const float depth = texture(depth_image, uv).r;

  // Глубина 0 = ничего не нарисовано (reverse-Z, бесконечная дальняя плоскость): это небо
  if (depth <= 0.0) {
    // Небо ЗНАЧИТЕЛЬНО ярче освещённых поверхностей, и это не подгонка под задачу: в реальности небо
    // измеряется тысячами кандел на квадратный метр против сотен у освещённой стены. Именно из-за него
    // диапазон кадра не влезает в 8 бит, и именно его сжимает кривая. Яркость масштабируется солнцем,
    // потому что небо светится тем же светом.
    const float sun_scale = frame.exposure_limits.w * 1.6;
    const vec3 sky = mix(vec3(0.35, 0.50, 0.95), vec3(1.10, 1.40, 2.00), clamp(1.0 - uv.y, 0.0, 1.0)) * sun_scale;
    imageStore(scene_image, pixel, vec4(sky, 1.0));
    return;
  }

  const vec3 normal = pf03_decode_normal(texture(normal_image, uv).rg);
  const vec3 world = world_position_from_depth(uv, depth);

  // Шейдинг намеренно простой: срез проверяет G-buffer и репроекцию, а не освещение. Достаточно, чтобы
  // картинка имела заметный контраст — иначе ошибку репроекции нечем мерить.
  const float ndl = max(dot(normal, normalize(frame.light_direction.xyz)), 0.0);
  // Ambient задан ДОЛЕЙ солнца, а не абсолютом: отражённый от неба свет масштабируется тем же светилом,
  // поэтому при смене яркости солнца сцена не рассыпается на «выжженный свет и чёрные тени».
  const float ambient = frame.light_direction.w * frame.exposure_limits.w;

  // Альбедо берётся ИЗ НОРМАЛИ, а не из мировых координат. Это принципиально для per-object motion: если
  // рисунок привязан к миру, он «плывёт» по движущейся поверхности, и точка меняет цвет между кадрами — тогда
  // даже идеальные motion-векторы дадут ошибку репроекции, и метрика будет мерить не то.
  const vec3 albedo = vec3(0.58, 0.56, 0.54) + normal * vec3(0.16, 0.10, 0.18);

  // Исключение — статичный пол: он никуда не движется, поэтому на нём оставлен жёсткий шахматный
  // стресс-паттерн для измерений motion ОТ КАМЕРЫ (и как будущая нагрузка на neighbourhood clamp в TAA).
  const bool is_floor = normal.y > 0.99 && world.y < -2.9;
  const float checker = is_floor && (step(0.5, fract(world.x * 0.5)) != step(0.5, fract(world.z * 0.5))) ? 0.68 : 1.0;

  // Солнце в физических единицах, а не «в единицах экрана»: диапазон кадра получается 1:100+, и дальше уже
  // экспозиция решает, что считать серым.
  const float sun = frame.exposure_limits.w;

  // Спекуляр нужен здесь не ради красоты: узкий яркий блик — это и есть настоящий свет, который в 8 бит не
  // влезает. Без него диффузная сцена целиком укладывается в диапазон, ничего не клиппится, и сравнивать
  // операторы tone mapping попросту не на чем.
  const vec3 to_camera = normalize(frame.camera_position.xyz - world);
  const vec3 half_vector = normalize(to_camera + normalize(frame.light_direction.xyz));
  // Показатель невысокий сознательно: узкий блик легко уходит за кадр, и тогда сравнивать операторы не на
  // чем. Широкий глянцевый блик на полу виден почти при любой камере и даёт пик на два порядка выше среднего —
  // именно такие пики экспозиция компенсировать не может (они почти не двигают среднее), а кривая обязана.
  const float specular = pow(max(dot(normal, half_vector), 0.0), 24.0) * sun * 30.0;

  // Светящаяся панель на дальней стене. Опознаётся по мировым координатам — для статичной геометрии это
  // законно, а сам шейдинг здесь всё равно заглушка. Нужна она затем, что автоэкспозиция нормирует СРЕДНЮЮ
  // яркость: небо она уложит под единицу, и сжимать станет нечего. А маленький источник на два порядка ярче
  // среднего среднее почти не двигает — и вот его-то экспозиция вытянуть уже не может, только кривая.
  const bool lamp = world.z < -8.85 && abs(world.x - 2.5) < 2.2 && world.y > -1.2 && world.y < 0.9;
  const vec3 emissive = lamp ? vec3(1.0, 0.92, 0.78) * sun * 40.0 : vec3(0.0);

  const vec3 color = albedo * checker * (ndl * sun + ambient) + vec3(specular) + emissive;
  imageStore(scene_image, pixel, vec4(color, 1.0));
}
