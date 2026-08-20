#version 450

#include "pf03_shading.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D depth_image;
layout(set = 1, binding = 1) uniform sampler2D normal_image;
layout(set = 1, binding = 2) uniform sampler2D motion_image;

layout(set = 2, binding = 0) uniform sampler2D ao_image;

layout(set = 3, binding = 0, rgba16f) uniform writeonly image2D scene_image;

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
    // Небо считается общей функцией: тот же цвет обязан получаться и в forward-ветви с MSAA
    const vec3 sky = pf03_sky_color(uv, frame.exposure_limits.w);
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
  const float ambient_light = frame.light_direction.w * frame.exposure_limits.w;

  // AO живёт в половинном разрешении, поэтому апсемпл depth-aware: из четырёх соседей берём того, чья
  // глубина ближе к нашей. Обычный bilinear протекает через силуэты и рисует затенение на фоне (урок PF02).
  float ao = 1.0;
  if (frame.ao_params.w > 0.0) {
    const ivec2 ao_size = textureSize(ao_image, 0);
    const vec2 ao_coord = uv * vec2(ao_size) - 0.5;
    const ivec2 base = ivec2(floor(ao_coord));

    float best_difference = 1.0e9;
    for (int y = 0; y <= 1; ++y) {
      for (int x = 0; x <= 1; ++x) {
        const ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), ao_size - 1);
        const vec2 value = texelFetch(ao_image, tap, 0).rg;
        if (value.g <= 0.0) {
          continue; // тексель неба
        }
        const float difference = abs(value.g - pf03_linear_depth(depth, frame.viewport_near.z));
        if (difference < best_difference) {
          best_difference = difference;
          ao = value.r;
        }
      }
    }
  }

  // Дальше — общая функция освещения: её же вызывает forward-пасс с MSAA. AO передаётся параметром, потому
  // что умножать им можно ТОЛЬКО ambient: прямой солнечный свет затеняется геометрией и тенями, а не
  // статистикой соседей.
  const vec3 color = pf03_shade_surface(
    world, normal, frame.camera_position.xyz, frame.light_direction.xyz,
    frame.exposure_limits.w, frame.light_direction.w, ao, frame.output_params.z);

  imageStore(scene_image, pixel, vec4(color, 1.0));
}
