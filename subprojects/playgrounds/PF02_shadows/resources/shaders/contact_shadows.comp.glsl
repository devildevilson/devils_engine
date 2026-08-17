#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Тиры качества screen-space ray: задаются specialization-константами шага, поэтому пресет меняет
// только pipeline, а marching-код остаётся один.
layout(constant_id = 0) const int contact_ray_steps = 8;
layout(constant_id = 1) const int contact_refine_steps = 3;

layout(set = 0, binding = 0, std140) uniform SceneBlock {
  mat4 view_projection;
  mat4 view;
  mat4 light_view_projection;
  vec4 camera_position;
  vec4 viewport_near;
  vec4 light_direction;
  vec4 shadow_params;
  vec4 filter_params;
  // x/y: начало и конец затухания по глубине камеры, z: ширина viewport-edge fade в uv.
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

layout(set = 1, binding = 0) uniform sampler2D depth_image[3];
layout(set = 2, binding = 0, rg16f) uniform writeonly image2D directional_contact_image[3];
layout(set = 2, binding = 1, rgba8) uniform writeonly image2D spot_contact_image[3];

vec3 reconstruct_world_position(const ivec2 pixel, const float reverse_depth) {
  const vec2 viewport = scene_data[0].viewport_near.xy;
  const vec2 uv = (vec2(pixel) + 0.5) / viewport;
  const vec2 ndc = uv * 2.0 - 1.0;
  const float view_depth = scene_data[0].viewport_near.z / max(reverse_depth, 0.000001);
  const float projection_y = scene_data[0].camera_position.w;
  const float projection_x = projection_y * viewport.y / viewport.x;
  const vec3 view_position = vec3(
    ndc.x * view_depth / projection_x,
    -ndc.y * view_depth / projection_y,
    -view_depth);
  return transpose(mat3(scene_data[0].view)) * view_position + scene_data[0].camera_position.xyz;
}

vec3 reconstruct_receiver_normal(
  const ivec2 pixel,
  const float center_depth,
  const vec3 center_position,
  out float receiver_footprint,
  out bool stable_receiver) {
  const ivec2 viewport = ivec2(scene_data[0].viewport_near.xy);
  const ivec2 left_pixel = max(pixel - ivec2(1, 0), ivec2(0));
  const ivec2 right_pixel = min(pixel + ivec2(1, 0), viewport - 1);
  const ivec2 up_pixel = max(pixel - ivec2(0, 1), ivec2(0));
  const ivec2 down_pixel = min(pixel + ivec2(0, 1), viewport - 1);
  const float left_depth = texelFetch(depth_image[0], left_pixel, 0).r;
  const float right_depth = texelFetch(depth_image[0], right_pixel, 0).r;
  const float up_depth = texelFetch(depth_image[0], up_pixel, 0).r;
  const float down_depth = texelFetch(depth_image[0], down_pixel, 0).r;
  // Гейт «здесь нет силуэта» обязан быть свободен от масштаба. Прежний порог сравнивал ПЕРВУЮ разность
  // линейной глубины с долей дистанции, а на скользящем полу или просто вдалеке один пиксель покрывает
  // очень много мировых единиц: первая разность там огромна сама по себе, и весь наклонный/дальний пол
  // отбраковывался как «нестабильный» — contact-тени на нём не считались вовсе.
  // Плоскость в reverse-Z линейна в экранных координатах, поэтому её ВТОРАЯ разность равна нулю при
  // любом наклоне и любой дистанции: сравниваем кривизну с локальным наклоном, а не с дистанцией.
  const bool have_neighbours = left_depth > 0.0 && right_depth > 0.0 && up_depth > 0.0 && down_depth > 0.0;
  const float curvature_x = abs(left_depth + right_depth - 2.0 * center_depth);
  const float curvature_y = abs(up_depth + down_depth - 2.0 * center_depth);
  const float slope_x = abs(right_depth - left_depth);
  const float slope_y = abs(down_depth - up_depth);
  const float curvature_limit = max(center_depth * 1.0e-3, 0.35 * max(slope_x, slope_y));
  stable_receiver = have_neighbours && max(curvature_x, curvature_y) <= curvature_limit;

  const float left_delta = abs(left_depth - center_depth);
  const float right_delta = abs(right_depth - center_depth);
  const float up_delta = abs(up_depth - center_depth);
  const float down_delta = abs(down_depth - center_depth);
  const vec3 dx = right_delta <= left_delta
    ? reconstruct_world_position(right_pixel, right_depth > 0.0 ? right_depth : center_depth) - center_position
    : center_position - reconstruct_world_position(left_pixel, left_depth > 0.0 ? left_depth : center_depth);
  const vec3 dy = down_delta <= up_delta
    ? reconstruct_world_position(down_pixel, down_depth > 0.0 ? down_depth : center_depth) - center_position
    : center_position - reconstruct_world_position(up_pixel, up_depth > 0.0 ? up_depth : center_depth);
  receiver_footprint = max(length(dx), length(dy));
  vec3 receiver_normal = cross(dx, dy);
  if (dot(receiver_normal, receiver_normal) < 1.0e-10) {
    receiver_normal = scene_data[0].camera_position.xyz - center_position;
  }
  receiver_normal = normalize(receiver_normal);
  if (dot(receiver_normal, scene_data[0].camera_position.xyz - center_position) < 0.0) {
    receiver_normal = -receiver_normal;
  }
  return receiver_normal;
}

bool contact_ray_hit(
  const vec3 receiver_position,
  const vec3 ray_origin,
  const vec3 receiver_normal,
  const vec3 surface_to_light,
  const float distance_along_ray,
  const float maximum_distance,
  const float thickness,
  const float plane_epsilon,
  out vec2 hit_uv) {
  hit_uv = vec2(0.5);
  const vec2 viewport = scene_data[0].viewport_near.xy;
  const vec3 sample_position = ray_origin + surface_to_light * distance_along_ray;
  const vec4 clip = scene_data[0].view_projection * vec4(sample_position, 1.0);
  if (clip.w <= 0.0) return false;
  const vec3 projected = clip.xyz / clip.w;
  const vec2 uv = projected.xy * 0.5 + 0.5;
  if (projected.z <= 0.0 || any(lessThanEqual(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) {
    return false;
  }
  hit_uv = uv;

  const ivec2 sample_pixel = clamp(ivec2(uv * viewport), ivec2(0), ivec2(viewport) - 1);
  const float stored_depth = texelFetch(depth_image[0], sample_pixel, 0).r;
  if (stored_depth <= 0.0) return false;
  const vec3 blocker_position = reconstruct_world_position(sample_pixel, stored_depth);
  const float blocker_plane_distance = dot(blocker_position - receiver_position, receiver_normal);
  if (blocker_plane_distance <= plane_epsilon) {
    // Only geometry on the illuminated side of the receiver plane may occlude it. Using abs() here
    // admits geometry behind the surface and produces the characteristic inverted contact wedges.
    return false;
  }

  const float surface_depth = scene_data[0].viewport_near.z / stored_depth;
  const float ray_depth = -(scene_data[0].view * vec4(sample_position, 1.0)).z;
  const float separation = ray_depth - surface_depth;
  const float fraction = clamp(distance_along_ray / maximum_distance, 0.0, 1.0);
  const float allowed_thickness = thickness * (1.0 + fraction);
  return separation > 0.008 && separation < allowed_thickness;
}

float contact_visibility(
  const vec3 receiver_position,
  const vec3 receiver_normal,
  const float receiver_footprint,
  const vec3 surface_to_light,
  const float maximum_distance,
  const float thickness,
  const float distance_fade) {
  if (dot(receiver_normal, surface_to_light) <= 0.001) return 1.0;
  if (distance_fade <= 0.0) return 1.0;
  const float start_distance = min(0.018, maximum_distance * 0.25);
  const float plane_epsilon = clamp(receiver_footprint * 1.25, 0.008, 0.035);
  const float normal_offset = clamp(plane_epsilon * 0.75, 0.006, 0.018);
  const vec3 ray_origin = receiver_position + receiver_normal * normal_offset;
  float previous_distance = start_distance;
  vec2 hit_uv = vec2(0.5);
  for (int step_index = 0; step_index < contact_ray_steps; ++step_index) {
    const float fraction = (float(step_index) + 0.75) / float(contact_ray_steps);
    const float distance_along_ray = mix(start_distance, maximum_distance, fraction);
    if (contact_ray_hit(
      receiver_position,
      ray_origin,
      receiver_normal,
      surface_to_light,
      distance_along_ray,
      maximum_distance,
      thickness,
      plane_epsilon,
      hit_uv)) {
      float clear_distance = previous_distance;
      float hit_distance = distance_along_ray;
      // Refine only occupied rays: this removes the eight visible distance bands without paying for
      // a uniformly denser march over the whole half-resolution buffer.
      for (int refinement = 0; refinement < contact_refine_steps; ++refinement) {
        const float candidate = (clear_distance + hit_distance) * 0.5;
        vec2 candidate_uv = hit_uv;
        if (contact_ray_hit(
          receiver_position,
          ray_origin,
          receiver_normal,
          surface_to_light,
          candidate,
          maximum_distance,
          thickness,
          plane_epsilon,
          candidate_uv)) {
          hit_distance = candidate;
          hit_uv = candidate_uv;
        } else {
          clear_distance = candidate;
        }
      }

      // Блокер у самого края экрана виден лишь частично, поэтому его вклад гасится: иначе на границе
      // кадра тень возникает и исчезает при повороте камеры.
      const float border = max(scene_data[0].contact_params.z, 0.0001);
      const vec2 border_distance = min(hit_uv, vec2(1.0) - hit_uv);
      const float edge_fade = clamp(min(border_distance.x, border_distance.y) / border, 0.0, 1.0);
      const float occlusion = (1.0 - smoothstep(0.0, maximum_distance, hit_distance)) *
        edge_fade * distance_fade;
      return 1.0 - clamp(occlusion, 0.0, 1.0);
    }
    previous_distance = distance_along_ray;
  }
  return 1.0;
}

void main() {
  const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  const ivec2 viewport = ivec2(scene_data[0].viewport_near.xy);
  const ivec2 output_size = imageSize(directional_contact_image[0]);
  if (any(greaterThanEqual(pixel, output_size))) return;
  const ivec2 source_pixel = min(pixel * 2 + ivec2(1), viewport - 1);

  const float reverse_depth = texelFetch(depth_image[0], source_pixel, 0).r;
  const float maximum_distance = max(scene_data[0].filter_params.z, 0.0);
  const float thickness = max(scene_data[0].filter_params.w, 0.001);
  // .g == 0 означает «в этом пикселе нет геометрии»: depth-aware upsample такой tap игнорирует.
  if (reverse_depth <= 0.0 || maximum_distance <= 0.0) {
    imageStore(directional_contact_image[0], pixel, vec4(1.0, 0.0, 0.0, 0.0));
    imageStore(spot_contact_image[0], pixel, vec4(1.0));
    return;
  }

  const float source_view_depth = scene_data[0].viewport_near.z / reverse_depth;
  const float fade_start = scene_data[0].contact_params.x;
  const float fade_end = scene_data[0].contact_params.y;
  const float distance_fade = fade_end > fade_start
    ? 1.0 - smoothstep(fade_start, fade_end, source_view_depth)
    : 1.0;
  if (distance_fade <= 0.0) {
    // Контакт — фича крупного плана: за пределами fade_end лучи не трассируются вообще.
    imageStore(directional_contact_image[0], pixel, vec4(1.0, source_view_depth, 0.0, 0.0));
    imageStore(spot_contact_image[0], pixel, vec4(1.0));
    return;
  }

  const vec3 world_position = reconstruct_world_position(source_pixel, reverse_depth);
  float receiver_footprint = 0.0;
  bool stable_receiver = false;
  const vec3 receiver_normal = reconstruct_receiver_normal(
    source_pixel,
    reverse_depth,
    world_position,
    receiver_footprint,
    stable_receiver);
  if (!stable_receiver) {
    imageStore(directional_contact_image[0], pixel, vec4(1.0, source_view_depth, 0.0, 0.0));
    imageStore(spot_contact_image[0], pixel, vec4(1.0));
    return;
  }
  const int lighting_mode = int(scene_data[0].viewport_near.w + 0.5);
  float directional = 1.0;
  if (lighting_mode != 2) {
    directional = contact_visibility(
      world_position,
      receiver_normal,
      receiver_footprint,
      normalize(-scene_data[0].light_direction.xyz),
      maximum_distance,
      thickness,
      distance_fade);
  }

  vec4 spots = vec4(1.0);
  if (lighting_mode != 1) {
    for (int index = 0; index < 4; ++index) {
      const vec3 to_light = spot_data[0].lights[index].position_range.xyz - world_position;
      const float distance_to_light = length(to_light);
      if (distance_to_light >= spot_data[0].lights[index].position_range.w) continue;
      const vec3 spot_to_surface = -to_light / max(distance_to_light, 0.0001);
      if (dot(spot_to_surface, spot_data[0].lights[index].direction_outer.xyz) <=
          spot_data[0].lights[index].direction_outer.w) continue;
      const float ray_distance = min(maximum_distance, distance_to_light);
      spots[index] = contact_visibility(
        world_position,
        receiver_normal,
        receiver_footprint,
        to_light / max(distance_to_light, 0.0001),
        ray_distance,
        thickness,
        distance_fade);
    }
  }

  imageStore(directional_contact_image[0], pixel, vec4(directional, source_view_depth, 0.0, 0.0));
  imageStore(spot_contact_image[0], pixel, spots);
}
