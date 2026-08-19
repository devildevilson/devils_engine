#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Тир качества: число проб. Смена стоит пересборки pipeline, а не нового шейдера.
layout(constant_id = 0) const int ao_samples = 16;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0, rg16f) uniform writeonly image2D ao_image;

vec3 world_from_depth(const vec2 uv, const float reverse_depth) {
  const vec4 ndc = vec4(uv * 2.0 - 1.0, reverse_depth, 1.0);
  const vec4 world = frame.inverse_view_projection * ndc;
  return world.xyz / world.w;
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(ao_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const float depth = texture(depth_image, uv).r;

  // Небо не затеняется: занулять там AO значило бы получить тёмную рамку по силуэтам на фоне неба
  if (depth <= 0.0) {
    imageStore(ao_image, pixel, vec4(1.0, 0.0, 0.0, 0.0));
    return;
  }

  const vec3 normal = pf03_decode_normal(texture(normal_image, uv).rg);
  const vec3 world = world_from_depth(uv, depth);
  const float view_depth = pf03_linear_depth(depth, frame.viewport_near.z);

  const float radius = frame.ao_params.x;
  const float tangent_bias = frame.ao_params.z;

  // Радиус в ЭКРАННЫХ единицах получаем честной проекцией точки, отстоящей на radius вдоль касательной, а не
  // приближением через фокусное расстояние: одна лишняя матричная умножение на пиксель, зато верно при любой
  // проекции и на любом расстоянии.
  const vec3 up_axis = abs(normal.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(up_axis, normal));
  const vec4 offset_clip = frame.view_projection * vec4(world + tangent * radius, 1.0);
  const vec2 offset_uv = offset_clip.w > 0.0 ? (offset_clip.xy / offset_clip.w) * 0.5 + 0.5 : uv;
  // Радиус ограничен сверху в ЭКРАННЫХ единицах. Без этого у нижнего края кадра, где пол виден под
  // скользящим углом, один пиксель покрывает огромную область мира: то же число проб размазывается по
  // большей площади, разброс оценки растёт, и AO покрывается веером радиальных штрихов. Ограничение заодно
  // держит цену эффекта: площадь выборки на пиксель перестаёт зависеть от геометрии.
  const float max_screen_radius = 0.04;
  const float screen_radius = clamp(length(offset_uv - uv), 1.0e-5, max_screen_radius);

  // Своё вращение выборки на каждый пиксель: один набор направлений на весь кадр даёт не шум, а structured
  // banding, который блюр только размажет, но не уберёт.
  // Временной сдвиг вращения: без него шум AO статичен, и накопление его не уберёт — усреднять нечего.
  // Именно эта пара «стохастическая выборка + темпоральное накопление» и была обещана в выводах PF02.
  const float rotation = pf03_gradient_noise(vec2(pixel)) + frame.taa_params.w;

  float occlusion = 0.0;
  for (int i = 0; i < ao_samples; ++i) {
    const vec2 xi = pf03_hammersley(uint(i), uint(ao_samples));
    const vec2 sample_uv = uv + pf03_disc_sample(xi, rotation) * screen_radius;
    if (any(lessThan(sample_uv, vec2(0.0))) || any(greaterThan(sample_uv, vec2(1.0)))) {
      continue;
    }

    const float sample_depth = texture(depth_image, sample_uv).r;
    if (sample_depth <= 0.0) {
      continue; // небо ничего не затеняет
    }

    // Ключевое отличие от сравнения глубин: занятость решается по ВОССТАНОВЛЕННОЙ позиции затенителя и его
    // положению относительно касательной плоскости. Компланарная поверхность даёт dot(v, n) около нуля и
    // отсекается смещением, поэтому открытый плоский пол не затеняет сам себя — а именно этим болеет
    // наивная версия на разнице глубин, причём тем сильнее, чем больше радиус.
    const vec3 occluder = world_from_depth(sample_uv, sample_depth);
    const vec3 to_occluder = occluder - world;
    const float distance_to_occluder = length(to_occluder);
    // Нижний порог расстояния масштабируется глубиной. На скользящих поверхностях соседние тексели глубины
    // лежат далеко в мире, и их восстановленные позиции шумят на пределе точности буфера — без этого отсечения
    // пол у нижнего края кадра покрывается крапчатым ложным затенением. Тот же приём, что мировой texel-offset
    // у теней в PF02: порог задаётся не в абсолютных метрах, а в масштабе того, что видно в пикселе.
    const float precision_floor = view_depth * 0.006;
    if (distance_to_occluder > radius || distance_to_occluder < precision_floor) {
      continue;
    }

    const float elevation = dot(to_occluder / distance_to_occluder, normal);
    // Вес падает с расстоянием: близкий затенитель перекрывает больше телесного угла
    const float falloff = 1.0 - distance_to_occluder / radius;
    occlusion += max(elevation - tangent_bias, 0.0) * falloff;
  }

  // Нормировка на число проб и на 0.5: косинус-взвешенное затенение полусферы даёт в среднем половину
  // единицы даже при полном перекрытии, и без множителя «сила = 1» выглядела бы как «сила = 0.5».
  const float linear_ao = 1.0 - clamp(occlusion / float(ao_samples) * 2.0, 0.0, 1.0) * frame.ao_params.y;

  // Показатель контраста. Он нужен не для красоты: оценщик ограничен радиусом и потому систематически
  // НЕДООЦЕНИВАЕТ затенение — у стены бесконечной высоты честный ответ 0.5 при любом расстоянии, а выборка
  // видит только затенители в пределах радиуса. Степень возвращает середину диапазона к правдоподобной,
  // не обрезая крайние значения, как это сделал бы простой множитель.
  const float ao = pow(clamp(linear_ao, 0.0, 1.0), max(frame.ao_params.w, 0.01));
  imageStore(ao_image, pixel, vec4(ao, view_depth, 0.0, 0.0));
}
