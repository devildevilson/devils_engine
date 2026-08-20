#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Способ марша — тир качества, поэтому specialization-константа, а не число в UBO: смена стоит пересборки
// pipeline. Линейный марш оставлен НЕ для продукта, а как эталон: только сравнив с ним, можно утверждать, что
// иерархический находит те же пересечения, а не «выглядит правдоподобно».
layout(constant_id = 0) const int ssr_hierarchical = 1;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0) uniform sampler2D scene_image; // затуманенный кадр: отражения наследуют аэроперспективу
layout(set = 2, binding = 1) uniform sampler2D hiz_image;

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D result_image;
// Диагностика марша отдельным ресурсом, а не в альфе кадра: в альфе живёт пропускание тумана, а без чисел про
// шаги и судьбу луча иерархический марш нечем сравнить с эталонным.
// .r = шагов / предел, .g = попадание, .b = луч ушёл за кадр, .a = вес отражения
layout(set = 3, binding = 1, rgba16f) uniform writeonly image2D stats_image;

vec3 world_position_from_depth(const vec2 uv, const float reverse_depth) {
  const vec4 ndc = vec4(uv * 2.0 - 1.0, reverse_depth, 1.0);
  const vec4 world = frame.inverse_view_projection * ndc;
  return world.xyz / world.w;
}

// Точка мира в экранные координаты: xy = uv, z = глубина reverse-Z. Возвращает w клипа, чтобы вызывающий мог
// отбросить точки за камерой.
vec4 world_to_screen(const vec3 world) {
  const vec4 clip = frame.view_projection * vec4(world, 1.0);
  if (clip.w <= 0.0) {
    return vec4(0.0, 0.0, 0.0, clip.w);
  }
  const vec3 ndc = clip.xyz / clip.w;
  return vec4(ndc.xy * 0.5 + 0.5, ndc.z, clip.w);
}

struct march_result {
  vec2 uv;
  float steps;   // сколько отсчётов понадобилось — это и есть цена марша
  bool hit;
  bool offscreen; // луч ушёл за кадр: не «нет отражения», а «экранный метод не знает»
};

// Толщина поверхности: без неё марш «находит» пересечение всюду, где луч ушёл ЗА объект и больше не вернулся,
// то есть рисует отражение переднего плана на всём, что за ним. Величина в линейной глубине, а не в reverse-Z:
// в reverse-Z один и тот же зазор у near и у горизонта означает совершенно разные расстояния.
bool depth_crossing(const float ray_depth, const float surface_depth, const float thickness) {
  if (surface_depth <= 0.0) {
    return false; // небо: поверхности здесь нет
  }
  if (ray_depth > surface_depth) {
    return false; // луч ближе поверхности — ещё не дошёл
  }
  const float ray_linear = pf03_linear_depth(ray_depth, frame.viewport_near.z);
  const float surface_linear = pf03_linear_depth(surface_depth, frame.viewport_near.z);
  return (ray_linear - surface_linear) < thickness;
}

// ЭТАЛОННЫЙ марш: равномерные шаги по экранному отрезку. Медленный и без всякой иерархии — именно поэтому он
// годится как эталон: ошибиться в нём почти негде.
march_result march_linear(const vec2 uv0, const float d0, const vec2 uv1, const float d1, const float thickness,
                          const float max_steps, const float jitter) {
  march_result r;
  r.uv = uv0;
  r.steps = 0.0;
  r.hit = false;
  r.offscreen = false;

  const float steps = max(max_steps, 1.0);
  for (float i = 1.0; i <= steps; i += 1.0) {
    const float t = (i - 1.0 + jitter) / steps;
    const vec2 uv = mix(uv0, uv1, t);
    const float ray_depth = mix(d0, d1, t);
    r.steps = i;

    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
      r.offscreen = true;
      return r;
    }

    const float surface = texture(depth_image, uv).r;
    if (depth_crossing(ray_depth, surface, thickness)) {
      r.uv = uv;
      r.hit = true;
      return r;
    }
  }
  return r;
}

// ИЕРАРХИЧЕСКИЙ марш по пирамиде глубины. Ключевое здесь — шаг идёт РОВНО ДО ГРАНИЦЫ КЛЕТКИ текущего уровня,
// а не фиксированной длиной. Первая версия шагала на 2^level пикселей, и это оказался не тот алгоритм: шаг
// приземлялся посреди клетки, та же клетка проверялась заново, а главное — утверждение «шаг безопасен»
// относилось к клетке КОНЦА шага, тогда как пройти он мог через другую, непроверенную. Измерение это и
// показало: такой марш выходил дороже простого линейного и при этом менее точным.
//
// Правильная форма: в клетке уровня L луч либо целиком ближе самой близкой поверхности блока (тогда клетка
// пропускается целиком и можно подняться выше), либо нет — тогда спускаемся, НЕ двигая луч. На нулевом уровне
// клетка равна пикселю, и тест превращается в обычную проверку пересечения. Ровно здесь и нужна
// консервативность пирамиды: «ближе самой близкой» — утверждение обо ВСЁМ блоке.
march_result march_hierarchical(const vec2 uv0, const float d0, const vec2 uv1, const float d1,
                                const float thickness, const float max_steps, const float jitter) {
  march_result r;
  r.uv = uv0;
  r.steps = 0.0;
  r.hit = false;
  r.offscreen = false;

  const int levels = int(frame.hiz_params.y + 0.5);
  const vec2 frame_size = vec2(textureSize(depth_image, 0));
  const vec2 p0 = uv0 * frame_size;
  const vec2 p1 = uv1 * frame_size;
  const vec2 delta = p1 - p0;
  const float span = max(length(delta), 1.0e-3);

  // Сдвиг на сотую пикселя: без него шаг «до границы» оставляет луч ровно на границе, и следующая итерация
  // берёт ту же клетку снова.
  const float epsilon = 0.01 / span;

  int level = 0;
  float t = jitter / span;

  for (float i = 1.0; i <= max(max_steps, 1.0); i += 1.0) {
    r.steps = i;

    const vec2 p = p0 + delta * t;
    const float cell = exp2(float(level));

    // t до выхода из клетки по каждой оси; у почти нулевой компоненты деление даёт бесконечность, и min сам
    // выбирает другую ось
    const vec2 boundary = vec2(
      delta.x > 0.0 ? (floor(p.x / cell) + 1.0) * cell : floor(p.x / cell) * cell,
      delta.y > 0.0 ? (floor(p.y / cell) + 1.0) * cell : floor(p.y / cell) * cell);
    const vec2 to_boundary = (boundary - p) / delta;
    const float next_t = min(t + max(min(to_boundary.x, to_boundary.y), 0.0) + epsilon, 1.0);

    const vec2 uv_enter = mix(uv0, uv1, t);
    const vec2 uv_exit = mix(uv0, uv1, next_t);
    if (any(lessThan(uv_exit, vec2(0.0))) || any(greaterThan(uv_exit, vec2(1.0)))) {
      r.offscreen = true;
      return r;
    }

    const ivec2 level_size = textureSize(hiz_image, level);
    const ivec2 tap = clamp(ivec2(uv_enter * vec2(level_size)), ivec2(0), level_size - 1);
    // .g — самая БЛИЗКАЯ поверхность блока (reverse-Z)
    const float closest = texelFetch(hiz_image, tap, level).g;

    // Глубина луча линейна по t, поэтому её минимум на отрезке — на одном из концов
    const float depth_enter = mix(d0, d1, t);
    const float depth_exit = mix(d0, d1, next_t);
    const float nearest_ray = min(depth_enter, depth_exit);

    if (closest <= 0.0 || nearest_ray > closest) {
      // Луч целиком ближе содержимого клетки: пересечения в ней нет, пропускаем всю и поднимаемся
      t = next_t;
      level = min(level + 1, levels - 1);
      if (next_t >= 1.0) {
        return r;
      }
      continue;
    }

    if (level > 0) {
      level -= 1; // возможное пересечение: смотрим мельче, НЕ двигая луч
      continue;
    }

    // Нулевой уровень: клетка это пиксель, тест настоящий
    if (depth_crossing(depth_exit, closest, thickness)) {
      r.uv = uv_exit;
      r.hit = true;
      return r;
    }
    t = next_t;
    if (next_t >= 1.0) {
      return r;
    }
  }

  return r;
}

// Небо для отражений. ВАЖНОЕ ОГРАНИЧЕНИЕ, и его надо называть: основное небо в шейдинге задано градиентом по
// ЭКРАНУ, а отражению нужен цвет по НАПРАВЛЕНИЮ — это разные функции, и они не совпадают. Правильный ответ —
// environment probe, то есть отдельная лаборатория; здесь взят тот же градиент, но по высоте направления, чтобы
// отражение неба хотя бы не было чёрным и не спорило по яркости.
vec3 reflection_sky(const vec3 direction) {
  const float sun_scale = frame.exposure_limits.w * 1.6;
  const float height = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
  return mix(vec3(1.10, 1.40, 2.00), vec3(0.35, 0.50, 0.95), height) * sun_scale;
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(result_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec4 scene = texture(scene_image, uv);
  const float depth = texture(depth_image, uv).r;

  const float intensity = frame.ssr_params.x;
  // Небо и выключенный эффект: кадр проходит насквозь БЕЗ изменений, включая альфу (в ней пропускание тумана)
  if (intensity <= 0.0 || depth <= 0.0) {
    imageStore(result_image, pixel, scene);
    imageStore(stats_image, pixel, vec4(0.0));
    return;
  }

  const vec3 normal = pf03_decode_normal(texture(normal_image, uv).rg);
  const vec3 world = world_position_from_depth(uv, depth);
  const vec3 to_camera = normalize(frame.camera_position.xyz - world);
  const vec3 direction = reflect(-to_camera, normal);

  const float roughness = clamp(frame.ssr_params.y, 0.0, 1.0);
  const float thickness = frame.ssr_params.z;
  const float max_steps = frame.ssr_params.w;

  // Отражение не должно светить назад в камеру и в саму поверхность
  const float facing = dot(direction, normal);
  if (facing <= 0.0) {
    imageStore(result_image, pixel, scene);
    imageStore(stats_image, pixel, vec4(0.0));
    return;
  }

  // Длина луча в мире: дальше отражения всё равно уходят за кадр, а шагов тратится больше
  const float ray_length = 24.0;
  const vec3 end_world = world + direction * ray_length;

  const vec4 start_screen = world_to_screen(world + normal * 0.02);
  const vec4 end_screen = world_to_screen(end_world);

  // Конец луча за камерой — отрезок в экранном пространстве не строится, честнее сразу отдать небо
  bool offscreen = end_screen.w <= 0.0 || start_screen.w <= 0.0;
  bool hit = false;
  vec2 hit_uv = uv;
  float steps = 0.0;

  if (!offscreen) {
    // Джиттер по кадру: начальное смещение шага меняется, поэтому пропущенные тонкие детали в разных кадрах
    // разные, и накопление TAA их усредняет. Тот же приём, что у выборки SSAO в срезе 5.
    const float jitter = fract(pf03_gradient_noise(vec2(pixel)) + frame.taa_params.w);

    const march_result r = ssr_hierarchical != 0
      ? march_hierarchical(start_screen.xy, start_screen.z, end_screen.xy, end_screen.z, thickness, max_steps, jitter)
      : march_linear(start_screen.xy, start_screen.z, end_screen.xy, end_screen.z, thickness, max_steps, jitter);
    hit = r.hit;
    hit_uv = r.uv;
    steps = r.steps;
    offscreen = r.offscreen;
  }

  // Френель по Шлику: у скользящих углов отражение почти зеркальное, в лоб — слабое. Без него отражение
  // выглядит как равномерная плёнка поверх кадра.
  const float fresnel = pow(1.0 - clamp(dot(to_camera, normal), 0.0, 1.0), 5.0);
  const float weight = mix(fresnel, 1.0, 0.06) * intensity * (1.0 - roughness);

  vec3 reflection = reflection_sky(direction);
  if (hit) {
    // Затухание к краям кадра: у самой границы отражение обрывается, и без плавного схода видна рамка —
    // это честная плата экранного метода, а не дефект
    const vec2 edge = smoothstep(vec2(0.0), vec2(0.08), hit_uv) * (1.0 - smoothstep(vec2(0.92), vec2(1.0), hit_uv));
    const float border = min(edge.x, edge.y);
    reflection = mix(reflection, texture(scene_image, hit_uv).rgb, border);
  }

  const vec3 result = scene.rgb + reflection * weight;

  // Диагностика в альфу не помещается (там пропускание тумана), поэтому отладочные виды считают марш сами.
  imageStore(result_image, pixel, vec4(result, scene.a));
  imageStore(stats_image, pixel, vec4(
    steps / max(max_steps, 1.0), hit ? 1.0 : 0.0, offscreen ? 1.0 : 0.0, weight));
}
