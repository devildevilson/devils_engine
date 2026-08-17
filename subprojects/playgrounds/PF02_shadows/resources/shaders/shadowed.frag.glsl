#version 450

// Тир качества PCF: задаётся specialization-константой шага (shader_constants), поэтому смена
// радиуса стоит только пересборки pipeline, а не нового SPIR-V варианта.
layout(constant_id = 0) const int pcf_radius = 1;

layout(location = 0) in vec3 world_position;
layout(location = 1) in vec3 world_normal;
layout(location = 2) in vec2 uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  mat4 view_projection;
  mat4 view;
  mat4 light_view_projection;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 light_direction;
  vec4 shadow_params;
  vec4 filter_params;
  // x/y: начало и конец затухания contact по глубине камеры, z: ширина viewport-edge fade.
  vec4 contact_params;
} scene_data[3];

struct SpotLight {
  mat4 light_view_projection;
  vec4 position_range;
  vec4 direction_outer;
  vec4 color_intensity;
  vec4 shadow_params;
};

layout(set = 0, binding = 1, std430) readonly buffer SpotLightBuffer {
  SpotLight lights[];
} spot_data[3];

struct DirectionalCascade {
  mat4 light_view_projection;
  vec4 split_depths;
  vec4 shadow_params;
};

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

vec2 receiver_plane_gradient(const vec2 shadow_uv, const float receiver_depth) {
  const vec2 uv_dx = dFdx(shadow_uv);
  const vec2 uv_dy = dFdy(shadow_uv);
  const float depth_dx = dFdx(receiver_depth);
  const float depth_dy = dFdy(receiver_depth);
  const float determinant = uv_dx.x * uv_dy.y - uv_dy.x * uv_dx.y;
  if (abs(determinant) < 1.0e-8) return vec2(0.0);
  return vec2(
    (depth_dx * uv_dy.y - depth_dy * uv_dx.y) / determinant,
    (-depth_dx * uv_dy.x + depth_dy * uv_dx.x) / determinant);
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
  const float n_dot_l) {
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

  const vec2 tile = vec2(float(cascade_index & 1), float(cascade_index >> 1));
  const vec2 atlas_uv = (local_uv + tile) * 0.5;
  const float resolution = scene_data[0].shadow_params.x;
  const vec2 texel = vec2(1.0 / resolution);
  const vec2 safe_min = tile * 0.5 + texel * 0.5;
  const vec2 safe_max = (tile + vec2(1.0)) * 0.5 - texel * 0.5;
  const vec2 depth_gradient = receiver_plane_gradient(atlas_uv, projected.z);
  const float receiver_plane_scale = directional_data[0].cascades[cascade_index].shadow_params.z;
  const int aa_mode = int(scene_data[0].light_direction.w + 0.5);
  const float aa_radius = clamp(scene_data[0].filter_params.x, 0.25, 4.0);
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
  int cascade_index = -1;
  for (int index = 0; index < 4; ++index) {
    if (view_depth <= directional_data[0].cascades[index].split_depths.y) {
      cascade_index = index;
      break;
    }
  }
  if (cascade_index < 0 || view_depth < 0.0) {
    cascade_tint = vec3(1.0);
    return 1.0;
  }

  const vec3 tint_colors[4] = vec3[](
    vec3(1.0, 0.42, 0.42),
    vec3(0.42, 1.0, 0.52),
    vec3(0.42, 0.62, 1.0),
    vec3(1.0, 0.82, 0.32));
  const float debug_tint = directional_data[0].cascades[0].split_depths.w;
  float visibility = directional_cascade_visibility(cascade_index, normal, n_dot_l);
  cascade_tint = mix(vec3(1.0), tint_colors[cascade_index], debug_tint);

  if (cascade_index < 3) {
    const float blend_start = directional_data[0].cascades[cascade_index].split_depths.z;
    const float split_far = directional_data[0].cascades[cascade_index].split_depths.y;
    const float blend = smoothstep(blend_start, split_far, view_depth);
    if (blend > 0.0) {
      const float next_visibility = directional_cascade_visibility(cascade_index + 1, normal, n_dot_l);
      visibility = mix(visibility, next_visibility, blend);
      const vec3 next_tint = mix(vec3(1.0), tint_colors[cascade_index + 1], debug_tint);
      cascade_tint = mix(cascade_tint, next_tint, blend);
    }
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
  const float resolution = scene_data[0].shadow_params.x;
  const float tile_resolution = resolution * 0.5;
  const float world_texel =
    2.0 * abs(unoffset_clip.w) * spot_data[0].lights[index].shadow_params.x / tile_resolution;
  const float normal_offset = world_texel *
    (scene_data[0].shadow_params.y + scene_data[0].shadow_params.z * (1.0 - n_dot_l));
  const vec3 receiver_position = position + normal * normal_offset;
  const vec4 clip = spot_data[0].lights[index].light_view_projection * vec4(receiver_position, 1.0);
  const vec3 projected = clip.xyz / clip.w;
  const vec2 local_uv = projected.xy * 0.5 + 0.5;
  if (projected.z < 0.0 || projected.z > 1.0 || any(lessThan(local_uv, vec2(0.0))) || any(greaterThan(local_uv, vec2(1.0)))) {
    return 1.0;
  }

  const vec2 tile = vec2(float(index & 1), float(index >> 1));
  const vec2 atlas_uv = (local_uv + tile) * 0.5;
  const vec2 texel = vec2(1.0 / resolution);
  const vec2 safe_min = tile * 0.5 + texel * 0.5;
  const vec2 safe_max = (tile + vec2(1.0)) * 0.5 - texel * 0.5;
  const vec2 depth_gradient = receiver_plane_gradient(atlas_uv, projected.z);
  const float receiver_plane_scale = spot_data[0].lights[index].shadow_params.z;
  const int aa_mode = int(scene_data[0].light_direction.w + 0.5);
  const float aa_radius = clamp(scene_data[0].filter_params.x, 0.25, 4.0);
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
