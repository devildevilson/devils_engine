#version 450

#include "pf02_records.glsl"

// Тир качества PCF: задаётся specialization-константой шага (shader_constants), поэтому смена
// радиуса стоит только пересборки pipeline, а не нового SPIR-V варианта.
layout(constant_id = 0) const int pcf_radius = 1;
// Минимальная ширина перехода в ЭКРАННЫХ пикселях и предел, до которого разрешено расширять шаг tap'ов.
layout(constant_id = 1) const float pcf_min_screen_pixels = 3.0;
layout(constant_id = 2) const float pcf_max_radius_scale = 8.0;

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  PF02_SCENE_BLOCK_BODY
} scene_data[3];


layout(set = 0, binding = 1, std430) readonly buffer SpotLightBuffer {
  SpotLight lights[];
} spot_data[3];


layout(set = 0, binding = 2, std430) readonly buffer DirectionalCascadeBuffer {
  DirectionalCascade cascades[];
} directional_data[3];

// Атласы читаются двумя способами: сравнивающий сэмплер отдаёт билинейную ДОЛЮ прошедших сравнение
// текселей (аппаратный PCF), а обычный нужен там, где требуется сырое значение глубины: hard-режим
// A/B и blocker search PCSS.
layout(set = 1, binding = 0) uniform sampler2DShadow directional_shadow_compare[3];
layout(set = 1, binding = 1) uniform sampler2DShadow spot_shadow_compare[3];
layout(set = 1, binding = 2) uniform sampler2D directional_shadow_image[3];
layout(set = 1, binding = 3) uniform sampler2D spot_shadow_atlas[3];
layout(set = 2, binding = 0) uniform sampler2D directional_contact_image[3];
layout(set = 2, binding = 1) uniform sampler2D spot_contact_image[3];

const vec2 poisson_disk[16] = vec2[](
  vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
  vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
  vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
  vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
  vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
  vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
  vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
  vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

mat2 shadow_rotation(const vec3 position) {
  const vec3 cell = floor(position * 8.0);
  const float angle = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831853;
  const float c = cos(angle);
  const float s = sin(angle);
  return mat2(c, -s, s, c);
}

float reverse_depth_distance(const float depth, const float near_plane, const float far_plane) {
  const float a = near_plane / (far_plane - near_plane);
  const float b = near_plane * far_plane / (far_plane - near_plane);
  return b / max(depth + a, 0.00001);
}

// Градиент глубины по atlas-uv вдоль плоскости receiver'а, посчитанный АНАЛИТИЧЕСКИ из Якобиана
// light-матрицы: для ortho он точен, для перспективы — первый порядок в точке выборки, чего достаточно,
// потому что коррекция линейна по смещению tap'а.
// Это замена экранных производных ПО РОБАСТНОСТИ, а не по картинке: кадр совпадает с dFdx/dFdy-версией
// побитово (проверено, AE = 0), но у производных есть два принципиальных изъяна. Первый: на строке смены
// индекса каскада соседние пиксели одного 2x2-квада проецируются в РАЗНЫЕ тайлы атласа, и производная
// там мусорная. Второй: spot-цикл вызывает эту функцию в расходящемся управлении (continue по
// cone/range), где производные по спецификации не определены вовсе.
vec2 receiver_plane_gradient(
  const mat4 light_matrix,
  const vec3 surface_normal,
  const vec3 projected,
  const float clip_w,
  const vec2 atlas_to_local) {
  if (abs(clip_w) < 1.0e-6) return vec2(0.0);
  const vec3 row0 = vec3(light_matrix[0][0], light_matrix[1][0], light_matrix[2][0]);
  const vec3 row1 = vec3(light_matrix[0][1], light_matrix[1][1], light_matrix[2][1]);
  const vec3 row2 = vec3(light_matrix[0][2], light_matrix[1][2], light_matrix[2][2]);
  const vec3 row3 = vec3(light_matrix[0][3], light_matrix[1][3], light_matrix[2][3]);
  // Производные проективного деления в точке выборки; для ortho row3 == 0 и это просто строки матрицы.
  const vec3 q0 = (row0 - projected.x * row3) / clip_w;
  const vec3 q1 = (row1 - projected.y * row3) / clip_w;
  const vec3 q2 = (row2 - projected.z * row3) / clip_w;

  // Любой ортонормированный базис плоскости receiver'а: результат от его выбора не зависит.
  const vec3 helper = abs(surface_normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  const vec3 tangent = normalize(cross(surface_normal, helper));
  const vec3 bitangent = cross(surface_normal, tangent);

  // Смещение в плоскости -> (du, dv) в local-uv (отсюда множитель 0.5 от ndc) и -> ddepth.
  const float a11 = 0.5 * dot(q0, tangent);
  const float a12 = 0.5 * dot(q0, bitangent);
  const float a21 = 0.5 * dot(q1, tangent);
  const float a22 = 0.5 * dot(q1, bitangent);
  const float c1 = dot(q2, tangent);
  const float c2 = dot(q2, bitangent);
  const float determinant = a11 * a22 - a12 * a21;
  if (abs(determinant) < 1.0e-12) return vec2(0.0);

  // g = A^{-T} c переводит (du, dv) в ddepth, затем local-uv -> atlas-uv.
  const vec2 gradient_local = vec2(a22 * c1 - a21 * c2, a11 * c2 - a12 * c1) / determinant;
  return gradient_local / max(atlas_to_local, vec2(1.0e-6));
}

float receiver_depth_at(
  const float center_depth,
  const vec2 depth_gradient,
  const vec2 sample_offset,
  const float receiver_plane_scale) {
  return center_depth +
    dot(depth_gradient, sample_offset) * clamp(receiver_plane_scale, 0.0, 2.0);
}

// Разделимый tent-вес: центральный tap весит больше краевых, поэтому переход читается сглаженным,
// а не как «ступенька усреднения». Радиус приходит specialization-константой, поэтому вес считается
// от него, а не из фиксированной таблицы.
// Фильтр задаётся В ТЕКСЕЛЯХ карты, а экранный размер текселя падает с дистанцией: у дальних каскадов
// он уходит ниже пикселя, футпринт становится субпиксельным и сглаживание перестаёт работать — край
// снова читается лесенкой при любом режиме AA. Поэтому шаг tap'ов масштабируется так, чтобы переход
// занимал не меньше pcf_min_screen_pixels пикселей. Учитывается и наклон поверхности: у пола под малым
// углом один пиксель покрывает много мировых единиц, и именно там лесенка заметна сильнее всего.
float screen_aware_radius(
  const float base_radius,
  const float world_texel,
  const float view_depth,
  const vec3 surface_normal) {
  if (pcf_min_screen_pixels <= 0.0 || world_texel <= 0.0 || view_depth <= 0.0) {
    return base_radius;
  }
  const float viewport_height = max(scene_data[0].viewport_near.y, 1.0);
  const float projection_y = max(scene_data[0].camera_position.w, 1.0e-4); // 1 / tan(fov/2)
  const vec3 view_direction = normalize(scene_data[0].camera_position.xyz - world_position);
  const float grazing = max(abs(dot(view_direction, surface_normal)), 0.08);
  const float world_per_pixel = 2.0 * view_depth / (projection_y * viewport_height) / grazing;
  const float texel_pixels = world_texel / max(world_per_pixel, 1.0e-6);
  const float footprint_texels = float(2 * pcf_radius + 1) * base_radius;
  const float footprint_pixels = max(footprint_texels * texel_pixels, 1.0e-6);
  const float scale = max(pcf_min_screen_pixels / footprint_pixels, 1.0);
  return base_radius * min(scale, pcf_max_radius_scale);
}

float tap_weight(const int x, const int y) {
  const float extent = float(pcf_radius) + 1.0;
  return (1.0 - abs(float(x)) / extent) * (1.0 - abs(float(y)) / extent);
}

float weighted_pcf_directional(
  const vec2 atlas_uv,
  const float receiver_depth,
  const vec2 depth_gradient,
  const vec2 texel,
  const float aa_radius,
  const float receiver_plane_scale,
  const vec2 safe_min,
  const vec2 safe_max) {
  float visible = 0.0;
  float total = 0.0;
  for (int y = -pcf_radius; y <= pcf_radius; ++y) {
    for (int x = -pcf_radius; x <= pcf_radius; ++x) {
      const vec2 sample_offset = vec2(x, y) * aa_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float reference = receiver_depth_at(
        receiver_depth, depth_gradient, sample_uv - atlas_uv, receiver_plane_scale);
      const float weight = tap_weight(x, y);
      visible += weight * texture(directional_shadow_compare[0], vec3(sample_uv, reference));
      total += weight;
    }
  }
  return visible / total;
}

float weighted_pcf_spot(
  const vec2 atlas_uv,
  const float receiver_depth,
  const vec2 depth_gradient,
  const vec2 texel,
  const float aa_radius,
  const float receiver_plane_scale,
  const vec2 safe_min,
  const vec2 safe_max) {
  float visible = 0.0;
  float total = 0.0;
  for (int y = -pcf_radius; y <= pcf_radius; ++y) {
    for (int x = -pcf_radius; x <= pcf_radius; ++x) {
      const vec2 sample_offset = vec2(x, y) * aa_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float reference = receiver_depth_at(
        receiver_depth, depth_gradient, sample_uv - atlas_uv, receiver_plane_scale);
      const float weight = tap_weight(x, y);
      visible += weight * texture(spot_shadow_compare[0], vec3(sample_uv, reference));
      total += weight;
    }
  }
  return visible / total;
}

float directional_cascade_visibility(
  const int cascade_index,
  const vec3 normal,
  const float n_dot_l,
  const float view_depth) {
  if (directional_data[0].cascades[0].shadow_params.y < 0.5) return 1.0;
  const float world_texel = directional_data[0].cascades[cascade_index].shadow_params.x;
  const float normal_offset = world_texel *
    (scene_data[0].shadow_params.y + scene_data[0].shadow_params.z * (1.0 - n_dot_l));
  const vec3 receiver_position = world_position + normal * normal_offset;
  const vec4 clip = directional_data[0].cascades[cascade_index].light_view_projection *
    vec4(receiver_position, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(local_uv, vec2(0.0))) || any(greaterThan(local_uv, vec2(1.0)))) {
    return 1.0;
  }

  const vec2 region_scale = directional_data[0].cascades[cascade_index].uv_scale_offset.xy;
  const vec2 region_offset = directional_data[0].cascades[cascade_index].uv_scale_offset.zw;
  const vec2 atlas_uv = local_uv * region_scale + region_offset;
  const vec2 texel = 1.0 / max(scene_data[0].shadow_layout.yz, vec2(1.0));
  const vec2 safe_min = region_offset + texel * 0.5;
  const vec2 safe_max = region_offset + region_scale - texel * 0.5;
  const vec2 depth_gradient = receiver_plane_gradient(
    directional_data[0].cascades[cascade_index].light_view_projection, normal, projected, clip.w, region_scale);
  const float receiver_plane_scale = directional_data[0].cascades[cascade_index].shadow_params.z;
  const int aa_mode = int(scene_data[0].light_direction.w + 0.5);
  const float aa_radius = screen_aware_radius(
    clamp(scene_data[0].filter_params.x, 0.25, 4.0), world_texel, view_depth, normal);
  if (aa_mode == 0) {
    const float stored_depth = texture(directional_shadow_image[0], clamp(atlas_uv, safe_min, safe_max)).r;
    return projected.z >= stored_depth ? 1.0 : 0.0;
  }

  if (aa_mode == 2) {
    const mat2 rotation = shadow_rotation(world_position);
    float visible = 0.0;
    for (int i = 0; i < 16; ++i) {
      const vec2 sample_offset = rotation * poisson_disk[i] * aa_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float reference = receiver_depth_at(
        projected.z, depth_gradient, sample_uv - atlas_uv, receiver_plane_scale);
      visible += texture(directional_shadow_compare[0], vec3(sample_uv, reference));
    }
    return visible / 16.0;
  }

  return weighted_pcf_directional(
    atlas_uv, projected.z, depth_gradient, texel, aa_radius, receiver_plane_scale, safe_min, safe_max);
}

float directional_visibility(
  const vec3 normal,
  const float n_dot_l,
  const float view_depth,
  out vec3 cascade_tint) {
  const int active_cascades = int(scene_data[0].shadow_layout.x + 0.5);
  int cascade_index = -1;
  for (int index = 0; index < active_cascades; ++index) {
    if (view_depth <= directional_data[0].cascades[index].split_depths.y) {
      cascade_index = index;
      break;
    }
  }
  if (cascade_index < 0 || view_depth < 0.0) {
    cascade_tint = vec3(1.0);
    return 1.0;
  }

  const vec3 tint_colors[6] = vec3[](
    vec3(1.0, 0.42, 0.42),
    vec3(0.42, 1.0, 0.52),
    vec3(0.42, 0.62, 1.0),
    vec3(1.0, 0.82, 0.32),
    vec3(0.85, 0.45, 1.0),
    vec3(0.40, 0.95, 0.95));
  const float debug_tint = directional_data[0].cascades[0].split_depths.w;
  float visibility = directional_cascade_visibility(cascade_index, normal, n_dot_l, view_depth);
  cascade_tint = mix(vec3(1.0), tint_colors[cascade_index], debug_tint);

  // Затухание у края последнего каскада: за ним теней нет вообще, и без плавного перехода край
  // читается как обрыв (именно это раздражало на коротких shadow_distance). Дальний каскад грубый,
  // поэтому терять его постепенно дешевле, чем пытаться дотянуть качество.
  const float fade_fraction = clamp(scene_data[0].shadow_layout.w, 0.0, 0.9);
  if (fade_fraction > 0.0) {
    const float shadow_far = directional_data[0].cascades[active_cascades - 1].split_depths.y;
    const float fade_start = shadow_far * (1.0 - fade_fraction);
    cascade_tint = mix(cascade_tint, vec3(1.0), smoothstep(fade_start, shadow_far, view_depth));
  }

  if (cascade_index + 1 < active_cascades) {
    const float blend_start = directional_data[0].cascades[cascade_index].split_depths.z;
    const float split_far = directional_data[0].cascades[cascade_index].split_depths.y;
    const float blend = smoothstep(blend_start, split_far, view_depth);
    if (blend > 0.0) {
      const float next_visibility = directional_cascade_visibility(cascade_index + 1, normal, n_dot_l, view_depth);
      visibility = mix(visibility, next_visibility, blend);
      const vec3 next_tint = mix(vec3(1.0), tint_colors[cascade_index + 1], debug_tint);
      cascade_tint = mix(cascade_tint, next_tint, blend);
    }
  }

  if (fade_fraction > 0.0) {
    const float shadow_far = directional_data[0].cascades[active_cascades - 1].split_depths.y;
    const float fade_start = shadow_far * (1.0 - fade_fraction);
    visibility = mix(visibility, 1.0, smoothstep(fade_start, shadow_far, view_depth));
  }
  return visibility;
}

float spot_visibility(
  const int index,
  const vec3 position,
  const vec3 normal,
  const float n_dot_l) {
  if (spot_data[0].lights[0].shadow_params.y < 0.5) return 1.0;
  const vec4 unoffset_clip = spot_data[0].lights[index].light_view_projection * vec4(position, 1.0);
  const float region_resolution = max(
    spot_data[0].lights[index].uv_scale_offset.x * scene_data[0].shadow_layout.y, 1.0);
  const float world_texel =
    2.0 * abs(unoffset_clip.w) * spot_data[0].lights[index].shadow_params.x / region_resolution;
  const float normal_offset = world_texel *
    (scene_data[0].shadow_params.y + scene_data[0].shadow_params.z * (1.0 - n_dot_l));
  const vec3 receiver_position = position + normal * normal_offset;
  const vec4 clip = spot_data[0].lights[index].light_view_projection * vec4(receiver_position, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(local_uv, vec2(0.0))) || any(greaterThan(local_uv, vec2(1.0)))) {
    return 1.0;
  }

  const vec2 region_scale = spot_data[0].lights[index].uv_scale_offset.xy;
  const vec2 region_offset = spot_data[0].lights[index].uv_scale_offset.zw;
  const vec2 atlas_uv = local_uv * region_scale + region_offset;
  const vec2 texel = 1.0 / max(scene_data[0].shadow_layout.yz, vec2(1.0));
  const vec2 safe_min = region_offset + texel * 0.5;
  const vec2 safe_max = region_offset + region_scale - texel * 0.5;
  const vec2 depth_gradient = receiver_plane_gradient(
    spot_data[0].lights[index].light_view_projection, normal, projected, clip.w, region_scale);
  const float receiver_plane_scale = spot_data[0].lights[index].shadow_params.z;
  const int aa_mode = int(scene_data[0].light_direction.w + 0.5);
  const float spot_view_depth = -(scene_data[0].view * vec4(position, 1.0)).z;
  const float aa_radius = screen_aware_radius(
    clamp(scene_data[0].filter_params.x, 0.25, 4.0), world_texel, spot_view_depth, normal);
  const float emitter_radius = max(scene_data[0].filter_params.y, 0.0);
  if (aa_mode == 0 && emitter_radius <= 0.0) {
    const float stored_depth = texture(spot_shadow_atlas[0], clamp(atlas_uv, safe_min, safe_max)).r;
    return projected.z >= stored_depth ? 1.0 : 0.0;
  }

  const mat2 rotation = shadow_rotation(position);
  if (emitter_radius > 0.0) {
    float blocker_depth = 0.0;
    float blocker_count = 0.0;
    const float search_radius = clamp(emitter_radius / max(world_texel, 0.0001), 1.0, 12.0);
    for (int i = 0; i < 16; ++i) {
      const vec2 sample_offset = rotation * poisson_disk[i] * search_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float stored_depth = texture(spot_shadow_atlas[0], sample_uv).r;
      const float receiver_depth = receiver_depth_at(
        projected.z,
        depth_gradient,
        sample_uv - atlas_uv,
        receiver_plane_scale);
      if (stored_depth > receiver_depth && stored_depth > 0.0) {
        blocker_depth += stored_depth;
        blocker_count += 1.0;
      }
    }
    if (blocker_count == 0.0) return 1.0;

    blocker_depth /= blocker_count;
    const float near_plane = 0.15;
    const float far_plane = spot_data[0].lights[index].position_range.w;
    const float receiver_distance = reverse_depth_distance(projected.z, near_plane, far_plane);
    const float blocker_distance = reverse_depth_distance(blocker_depth, near_plane, far_plane);
    const float penumbra_world = emitter_radius *
      max(receiver_distance - blocker_distance, 0.0) / max(blocker_distance, 0.0001);
    const float filter_radius = clamp(max(aa_radius, penumbra_world / max(world_texel, 0.0001)), 0.5, 16.0);
    float visible = 0.0;
    for (int i = 0; i < 16; ++i) {
      const vec2 sample_offset = rotation * poisson_disk[i] * filter_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float reference = receiver_depth_at(
        projected.z, depth_gradient, sample_uv - atlas_uv, receiver_plane_scale);
      visible += texture(spot_shadow_compare[0], vec3(sample_uv, reference));
    }
    return visible / 16.0;
  }

  if (aa_mode == 2) {
    float visible = 0.0;
    for (int i = 0; i < 16; ++i) {
      const vec2 sample_offset = rotation * poisson_disk[i] * aa_radius * texel;
      const vec2 sample_uv = clamp(atlas_uv + sample_offset, safe_min, safe_max);
      const float reference = receiver_depth_at(
        projected.z, depth_gradient, sample_uv - atlas_uv, receiver_plane_scale);
      visible += texture(spot_shadow_compare[0], vec3(sample_uv, reference));
    }
    return visible / 16.0;
  }

  return weighted_pcf_spot(
    atlas_uv, projected.z, depth_gradient, texel, aa_radius, receiver_plane_scale, safe_min, safe_max);
}

vec3 surface_albedo(const vec3 normal) {
  if (normal.y > 0.8) {
    const ivec2 cell = ivec2(floor(world_position.xz));
    const float checker = ((cell.x + cell.y) & 1) == 0 ? 0.62 : 0.78;
    return vec3(checker, checker * 0.97, checker * 0.90);
  }
  if (normal.z > 0.8) return vec3(0.55, 0.61, 0.70);
  return vec3(0.72, 0.48, 0.30);
}

// Маски контакта живут в половинном разрешении. Обычный bilinear смешивает соседние пиксели через
// силуэт и даёт гало вокруг объектов, поэтому tap выбирается по БЛИЖАЙШЕЙ глубине: compute пишет в .g
// линейную view-глубину своего источника, а .g == 0 помечает пиксель без геометрии.
ivec2 nearest_contact_texel(const vec2 contact_uv, const float view_depth) {
  const ivec2 mask_size = textureSize(directional_contact_image[0], 0);
  const vec2 coordinate = contact_uv * vec2(mask_size) - 0.5;
  const ivec2 base_texel = ivec2(floor(coordinate));

  ivec2 best_texel = clamp(base_texel, ivec2(0), mask_size - 1);
  float best_difference = 1.0e30;
  for (int index = 0; index < 4; ++index) {
    const ivec2 texel = clamp(
      base_texel + ivec2(index & 1, index >> 1), ivec2(0), mask_size - 1);
    const float source_depth = texelFetch(directional_contact_image[0], texel, 0).g;
    if (source_depth <= 0.0) {
      continue;
    }
    const float difference = abs(source_depth - view_depth);
    if (difference < best_difference) {
      best_difference = difference;
      best_texel = texel;
    }
  }
  return best_texel;
}

void main() {
  const vec3 normal = normalize(world_normal);
  const vec3 albedo = surface_albedo(normal);
  const vec3 view_direction = normalize(scene_data[0].camera_position.xyz - world_position);
  const int lighting_mode = int(scene_data[0].viewport_near.w + 0.5);
  const float view_depth = -(scene_data[0].view * vec4(world_position, 1.0)).z;
  const vec2 contact_uv = gl_FragCoord.xy / scene_data[0].viewport_near.xy;
  const ivec2 contact_texel = nearest_contact_texel(contact_uv, view_depth);
  const float directional_contact = texelFetch(directional_contact_image[0], contact_texel, 0).r;
  const vec4 spot_contacts = texelFetch(spot_contact_image[0], contact_texel, 0);
  const vec3 ambient = albedo * 0.055;
  vec3 direct = vec3(0.0);
  if (lighting_mode != 2) {
    const vec3 to_light = normalize(-scene_data[0].light_direction.xyz);
    const float diffuse = max(dot(normal, to_light), 0.0);
    vec3 cascade_tint;
    const float visibility = directional_visibility(normal, diffuse, view_depth, cascade_tint);
    const vec3 half_direction = normalize(to_light + view_direction);
    const float specular = pow(max(dot(normal, half_direction), 0.0), 48.0) * step(0.0001, diffuse);
    // Умножение затемняло бы полутень дважды: карта уже отдаёт долю видимости. Контакт лишь
    // ограничивает её сверху, поэтому он не в состоянии осветлить то, что карта считает тенью.
    const float shadowed = min(visibility, directional_contact);
    direct = (albedo * diffuse + vec3(0.16) * specular) *
      vec3(1.0, 0.93, 0.78) * cascade_tint * 1.35 * shadowed;
  }

  vec3 spot_lighting = vec3(0.0);
  for (int index = 0; index < 4 && lighting_mode != 1; ++index) {
    const vec3 to_light_vector = spot_data[0].lights[index].position_range.xyz - world_position;
    const float distance_to_light = length(to_light_vector);
    const float range = spot_data[0].lights[index].position_range.w;
    if (distance_to_light >= range) continue;
    const vec3 spot_to_surface = -to_light_vector / max(distance_to_light, 0.0001);
    const float cone_cosine = dot(spot_to_surface, spot_data[0].lights[index].direction_outer.xyz);
    const float outer_cosine = spot_data[0].lights[index].direction_outer.w;
    const float cone = smoothstep(outer_cosine, min(outer_cosine + 0.10, 0.999), cone_cosine);
    if (cone <= 0.0) continue;
    const vec3 surface_to_light = -spot_to_surface;
    const float spot_diffuse = max(dot(normal, surface_to_light), 0.0);
    const float edge = max(1.0 - distance_to_light / range, 0.0);
    const float attenuation = edge * edge * cone;
    const float spot_shadow = spot_visibility(index, world_position, normal, spot_diffuse);
    const vec3 spot_half = normalize(surface_to_light + view_direction);
    const float spot_specular = pow(max(dot(normal, spot_half), 0.0), 48.0) * step(0.0001, spot_diffuse);
    const float spot_shadowed = min(spot_shadow, spot_contacts[index]);
    spot_lighting += (albedo * spot_diffuse + vec3(0.12) * spot_specular) *
      spot_data[0].lights[index].color_intensity.rgb *
      spot_data[0].lights[index].color_intensity.a * attenuation * spot_shadowed;
  }

  const vec3 hdr = ambient + direct + spot_lighting;
  out_color = vec4(hdr / (hdr + vec3(1.0)), 1.0);
}
