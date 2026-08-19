#version 450

#include "pf03_frame.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Первый уровень пирамиды делает ещё и яркий проход. Различать варианты specialization-константой дешевле,
// чем держать два почти одинаковых шейдера: код один, а pipeline два.
layout(constant_id = 0) const int bloom_prefilter = 0;

layout(set = 0, binding = 0, std140) uniform FrameBlock {
  PF03_FRAME_BLOCK_BODY
} frame;

layout(set = 1, binding = 0) uniform sampler2D source_image;
layout(set = 1, binding = 1, rgba16f) uniform writeonly image2D target_image;
layout(set = 1, binding = 2, rgba16f) uniform readonly image2D exposure_state;

// Мягкое колено порога: жёсткое отсечение по яркости даёт видимую границу, на которой свечение включается
// скачком, и на движении она мерцает. Колено переводит включение в плавный участок.
vec3 prefilter(const vec3 color, const float exposure) {
  const float threshold = frame.bloom_params.y;
  const float knee = max(frame.bloom_params.z, 1.0e-4);
  // Яркость измеряется в единицах ПОСЛЕ экспозиции, а сам цвет остаётся линейным: так порог означает
  // «ярче, чем зритель считает белым», и не приходится подгонять его под абсолютную яркость сцены.
  const float brightness = max(max(color.r, color.g), color.b) * exposure;
  const float soft = clamp((brightness - threshold + knee) / (2.0 * knee), 0.0, 1.0);
  const float weight = max(brightness - threshold, 0.0);
  const float contribution = max(soft * soft * knee, weight) / max(brightness, 1.0e-4);
  return color * clamp(contribution, 0.0, 1.0);
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 size = imageSize(target_image);
  if (pixel.x >= size.x || pixel.y >= size.y) {
    return;
  }

  const vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
  const vec2 texel = 1.0 / vec2(textureSize(source_image, 0));

  // Тринадцативыборочное понижение (Jimenez): обычный box на каждом уровне пропускает высокие частоты, и на
  // движении пирамида начинает пульсировать. Здесь центральный крест и четыре угловых квадрата с весами,
  // подобранными так, чтобы фильтр гасил частоты выше нового Найквиста.
  vec3 result = texture(source_image, uv).rgb * 0.125;
  result += texture(source_image, uv + texel * vec2(-1.0, -1.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2( 1.0, -1.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2(-1.0,  1.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2( 1.0,  1.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2(-2.0,  0.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2( 2.0,  0.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2( 0.0, -2.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2( 0.0,  2.0)).rgb * 0.0625;
  result += texture(source_image, uv + texel * vec2(-1.0,  0.0)).rgb * 0.125;
  result += texture(source_image, uv + texel * vec2( 1.0,  0.0)).rgb * 0.125;
  result += texture(source_image, uv + texel * vec2( 0.0, -1.0)).rgb * 0.125;
  result += texture(source_image, uv + texel * vec2( 0.0,  1.0)).rgb * 0.125;

  if (bloom_prefilter != 0) {
    result = prefilter(result, imageLoad(exposure_state, ivec2(0)).y);
  }

  imageStore(target_image, pixel, vec4(result, 1.0));
}
