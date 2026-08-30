#version 450

layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 planet_to_world;
  mat4 world_to_planet;
  vec4 camera_position;
  vec4 light_direction;
  vec4 border_colour;
  uvec4 params;
  vec4 viewport_near;
  mat4 inverse_view_projection;
} camera_data;

struct HydrologyFeature { vec4 a_direction_height; vec4 b_direction_height; vec4 widths_kind; };
layout(set = 0, binding = 1, std430) readonly buffer HydrologyFeatures { HydrologyFeature features[]; } hydrology;

layout(location = 0) out vec2 out_feature_coordinate;
layout(location = 1) out float out_half_width;
layout(location = 2) flat out float out_kind;
layout(location = 3) flat out float out_variant;

void main() {
  const HydrologyFeature feature = hydrology.features[gl_InstanceIndex];
  const vec2 corners[6] = vec2[6](vec2(0, -1), vec2(1, -1), vec2(1, 1),
                                   vec2(0, -1), vec2(1, 1), vec2(0, 1));
  const vec2 corner = corners[gl_VertexIndex];
  const vec3 a = normalize(feature.a_direction_height.xyz);
  const vec3 b = normalize(feature.b_direction_height.xyz);
  const vec3 middle = normalize(a + b);
  vec3 along = b - a;
  if (dot(along, along) < 1e-10) {
    along = normalize(cross(middle, abs(middle.y) < 0.8 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
  }
  const vec3 along_unit = normalize(along);
  const vec3 across = normalize(cross(middle, along_unit));
  const bool lake = feature.widths_kind.z > 0.5;
  const float half_width = lake ? feature.widths_kind.x :
                                  mix(feature.widths_kind.x, feature.widths_kind.y, corner.x);
  const float bound = half_width + 0.00038;
  vec3 local_position;
  if (lake) {
    const vec2 disc_coordinate = vec2(corner.x * 2.0 - 1.0, corner.y) * bound;
    local_position = a * (1.0 + feature.a_direction_height.w) +
                     along_unit * disc_coordinate.x + across * disc_coordinate.y;
    out_feature_coordinate = disc_coordinate;
  } else {
    local_position = mix(a * (1.0 + feature.a_direction_height.w),
                         b * (1.0 + feature.b_direction_height.w), corner.x) +
                     across * corner.y * bound;
    out_feature_coordinate = vec2(0.0, corner.y * bound);
  }
  // A tiny radial lift prevents coplanar fighting; the conservative horizon gate rejects the hidden sphere.
  local_position += normalize(local_position) * 0.00075;
  const vec3 local_eye = (camera_data.world_to_planet * vec4(camera_data.camera_position.xyz, 1.0)).xyz;
  const float conservative_horizon = 0.95 / length(local_eye) - 0.012;
  gl_Position = dot(middle, normalize(local_eye)) < conservative_horizon ?
                  vec4(2.0, 2.0, 2.0, 1.0) :
                  camera_data.view_projection * camera_data.planet_to_world * vec4(local_position, 1.0);
  out_half_width = half_width;
  out_kind = feature.widths_kind.z;
  out_variant = feature.widths_kind.w;
}
